#!/usr/bin/env python3
"""atnode_broker.py — single-file remote broker for ESP32 AT-Node devices.

Runs on a remote server and provides:

  1. An embedded MQTT broker (amqtt) with TLS + password auth.
     ESP32 devices connect out to it (works through NAT).
  2. An HTTP proxy API (Bearer token). Users/agents curl the HTTP API
     to securely reach any connected device:
       GET  /api/help
       GET  /api/devices
       GET  /api/devices/<id>
       POST /api/devices/<id>/cmd/<method>?k=v&...
  3. A client CLI (list devices / show services / call methods).

MQTT wire protocol (device side is esp32_at_node firmware):
  atnode/<id>/state   retained "online"/"offline" (LWT)
  atnode/<id>/info    retained JSON manifest (services catalog)
  atnode/<id>/cmd     "<reqid> <method> <urlencoded query>"
  atnode/<id>/resp    {"id":..,"ok":.., ...}

Usage:
  atnode_broker.py serve [--mqtt-port 1883] [--mqtt-tls-port 8883]
                         [--certs DIR] [--http [PORT]]
                         (HTTP proxy is OPT-IN: only started with --http)
  atnode_broker.py client list
  atnode_broker.py client info <device>
  atnode_broker.py client call <device> <method> [k=v ...]
  atnode_broker.py client wol  <device> <mac>
  atnode_broker.py client ping <device> <host> [count]

Config (auto-created on first serve): ~/.atnode_broker.json
  {"token": ..., "mqtt_user": ..., "mqtt_password": ...}
Point the ESP32 at this broker:
  AT+MQTT=broker,<server-ip>  AT+MQTT=port,8883  AT+MQTT=ca,<fingerprint>
  (username/password via AT+CONF=mqtt_user=..., AT+CONF=mqtt_pass=...)
"""

import argparse
import asyncio
import json
import os
import secrets
import ssl
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CONFIG_PATH = os.environ.get("ATNODE_BROKER_CONFIG",
                             os.path.expanduser("~/.atnode_broker.json"))

HELP_TEXT = """AT-Node remote broker API

Auth: header  Authorization: Bearer <token>

  GET  /api/help                          this text (no auth)
  GET  /api/devices                       list devices + online state
  GET  /api/devices/<id>                  device detail + services + usage
  POST /api/devices/<id>/cmd/<method>     run a device method
       params via query string, form body, or JSON body

Common methods (see /api/devices/<id> for the live catalog):
  keyboard/tap    mods,k,ms        single key press+release
  keyboard/text   s,ms,gap         type ASCII text
  keyboard/key    mods,k0..k5      raw HID report state
  gpio/write      pin,level
  gpio/read       pin
  adc/read        ch
  ble/status                       BLE name/addr/peers/bonds
  net/wol         mac              Wake-on-LAN magic packet (device LAN)
  net/ping        host,count       ICMP ping from device LAN
  sys/info                         device manifest

Examples:
  curl -H "Authorization: Bearer $TOK" http://server:8080/api/devices
  curl -X POST -H "Authorization: Bearer $TOK" \\
       "http://server:8080/api/devices/atnodeesp-5688/cmd/keyboard/text?s=Hello"
  curl -X POST -H "Authorization: Bearer $TOK" \\
       "http://server:8080/api/devices/atnodeesp-5688/cmd/net/wol?mac=AA:BB:CC:DD:EE:FF"
"""


# ---------------------------------------------------------------- config

def load_or_create_config(args):
    cfg = {}
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg = json.load(f)
    changed = False
    for key, gen in (("token", lambda: secrets.token_hex(16)),
                     ("mqtt_user", lambda: "atnode"),
                     ("mqtt_password", lambda: secrets.token_hex(12))):
        if not cfg.get(key):
            cfg[key] = gen()
            changed = True
    if args.token:
        cfg["token"] = args.token
    if changed or args.token:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
        try:
            os.chmod(CONFIG_PATH, 0o600)
        except OSError:
            pass
    return cfg


# ---------------------------------------------------------------- mqtt broker (amqtt, asyncio thread)

def start_amqtt(cfg, mqtt_port, mqtt_tls_port, certs_dir, loop):
    from amqtt.broker import Broker
    from passlib.apps import custom_app_context as pwd_context

    pw_file = os.path.join(os.path.dirname(CONFIG_PATH) or ".",
                           ".atnode_broker_passwd")
    with open(pw_file, "w", encoding="utf-8") as f:
        f.write("%s:%s\n" % (cfg["mqtt_user"],
                             pwd_context.hash(cfg["mqtt_password"])))

    listeners = {
        "default": {"type": "tcp", "bind": f"0.0.0.0:{mqtt_port}"},
    }
    cert = os.path.join(certs_dir, "server.crt") if certs_dir else None
    key = os.path.join(certs_dir, "server.key") if certs_dir else None
    if cert and key and os.path.exists(cert) and os.path.exists(key):
        listeners["tls"] = {
            "type": "tcp", "bind": f"0.0.0.0:{mqtt_tls_port}",
            "ssl": True, "certfile": cert, "keyfile": key,
        }

    config = {
        "listeners": listeners,
        "plugins": {
            "amqtt.plugins.authentication.AnonymousAuthPlugin": {
                "allow_anonymous": False,
            },
            "amqtt.plugins.authentication.FileAuthPlugin": {
                "password_file": pw_file,
            },
            "amqtt.plugins.sys.broker.BrokerSysPlugin": {
                "sys_interval": 20,
            },
        },
    }

    async def run():
        broker = Broker(config)
        await broker.start()
        binds = [l["bind"] for l in config["listeners"].values()]
        print(f"[mqtt] broker listening: {', '.join(binds)}")
        await asyncio.Event().wait()

    loop.run_until_complete(run())


# ---------------------------------------------------------------- bridge (paho, thread)

class Bridge:
    """Internal MQTT client: device registry + RPC correlation."""

    def __init__(self, cfg, mqtt_port):
        import paho.mqtt.client as mqtt

        self.devices = {}            # id -> {"online": bool, "info": dict, "last_seen": ts}
        self.pending = {}            # reqid -> {"event": Event, "resp": dict}
        self.lock = threading.Lock()

        self.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                client_id="atnode-broker-bridge")
        self.mqtt.username_pw_set(cfg["mqtt_user"], cfg["mqtt_password"])
        self.mqtt.on_connect = self._on_connect
        self.mqtt.on_message = self._on_message
        self.mqtt.connect("127.0.0.1", mqtt_port, keepalive=30)
        self.mqtt.loop_start()

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        client.subscribe("atnode/+/state")
        client.subscribe("atnode/+/info")
        client.subscribe("atnode/+/resp")
        print("[bridge] connected to embedded broker")

    def _on_message(self, client, userdata, msg):
        parts = msg.topic.split("/")
        if len(parts) != 3 or parts[0] != "atnode":
            return
        dev, kind = parts[1], parts[2]
        payload = msg.payload.decode("utf-8", "replace")
        with self.lock:
            if kind == "state":
                d = self.devices.setdefault(dev, {})
                d["online"] = payload.strip() == "online"
                d["last_seen"] = time.time()
            elif kind == "info":
                d = self.devices.setdefault(dev, {})
                try:
                    d["info"] = json.loads(payload)
                except json.JSONDecodeError:
                    d["info"] = None
                d["last_seen"] = time.time()
            elif kind == "resp":
                try:
                    data = json.loads(payload)
                except json.JSONDecodeError:
                    return
                reqid = data.get("id")
                p = self.pending.get(reqid)
                if p:
                    p["resp"] = data
                    p["event"].set()

    def rpc(self, dev, method, params, timeout=10.0):
        reqid = uuid.uuid4().hex[:12]
        ev = threading.Event()
        with self.lock:
            self.pending[reqid] = {"event": ev, "resp": None}
        query = urllib.parse.urlencode(params or {})
        self.mqtt.publish(f"atnode/{dev}/cmd", f"{reqid} {method} {query}", qos=0)
        ok = ev.wait(timeout)
        with self.lock:
            p = self.pending.pop(reqid, None)
        if not ok:
            return {"ok": False, "error": f"timeout waiting for {dev} ({timeout}s)"}
        return p["resp"]


# ---------------------------------------------------------------- HTTP proxy

class Handler(BaseHTTPRequestHandler):
    server_version = "ATNodeBroker/1.0"

    def log_message(self, fmt, *a):
        print("[http]", fmt % a, flush=True)

    # -- helpers
    def _send(self, code, obj, ctype="application/json"):
        body = obj.encode() if isinstance(obj, str) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authed(self):
        h = self.headers.get("Authorization", "")
        return h == f"Bearer {self.server.cfg['token']}"

    def _params(self, url):
        params = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length:
            raw = self.rfile.read(length)
            ct = self.headers.get("Content-Type", "")
            if "json" in ct:
                try:
                    params.update({k: str(v) for k, v in json.loads(raw).items()})
                except json.JSONDecodeError:
                    pass
            else:
                params.update({k: v[0] for k, v in
                               urllib.parse.parse_qs(raw.decode()).items()})
        return params

    # -- routes
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/api/help":
            return self._send(200, HELP_TEXT, "text/plain; charset=utf-8")
        if not self._authed():
            return self._send(401, {"ok": False, "error": "unauthorized"})
        if url.path == "/api/devices":
            with self.server.bridge.lock:
                devs = [{"id": k,
                         "online": v.get("online", False),
                         "info": v.get("info"),
                         "last_seen": v.get("last_seen")}
                        for k, v in sorted(self.server.bridge.devices.items())]
            return self._send(200, {"devices": devs})
        if url.path.startswith("/api/devices/"):
            dev = url.path.split("/")[3]
            with self.server.bridge.lock:
                d = self.server.bridge.devices.get(dev)
            if not d:
                return self._send(404, {"ok": False, "error": "unknown device"})
            info = d.get("info") or {}
            return self._send(200, {
                "id": dev,
                "online": d.get("online", False),
                "info": info,
                "services": info.get("services", []),
                "usage": {
                    "call": f"POST /api/devices/{dev}/cmd/<method>?k=v&...",
                    "examples": [
                        f"POST /api/devices/{dev}/cmd/keyboard/text?s=Hello",
                        f"POST /api/devices/{dev}/cmd/net/wol?mac=AA:BB:CC:DD:EE:FF",
                        f"POST /api/devices/{dev}/cmd/net/ping?host=192.168.1.1&count=4",
                    ],
                },
            })
        return self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        if not self._authed():
            return self._send(401, {"ok": False, "error": "unauthorized"})
        parts = url.path.strip("/").split("/")
        # /api/devices/<id>/cmd/<method...>
        if len(parts) >= 5 and parts[0] == "api" and parts[1] == "devices" \
                and parts[3] == "cmd":
            dev = parts[2]
            method = "/".join(parts[4:])
            params = self._params(url)
            return self._send(200, self.server.bridge.rpc(dev, method, params))
        return self._send(404, {"ok": False, "error": "not found"})


# ---------------------------------------------------------------- client CLI

def client_request(server, token, method, path, params=None):
    if params:
        path += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(server.rstrip("/") + path, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read())


def run_client(args):
    cfg = {}
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg = json.load(f)
    server = args.server or "http://127.0.0.1:8080"
    token = args.token or cfg.get("token", "")

    if args.what == "list":
        data = client_request(server, token, "GET", "/api/devices")
        devs = data.get("devices", [])
        if not devs:
            print("no devices seen yet")
            return
        print(f"{'ID':24} {'STATE':8} {'IP':16} NAME")
        for d in devs:
            info = d.get("info") or {}
            print(f"{d['id']:24} "
                  f"{'online' if d.get('online') else 'offline':8} "
                  f"{info.get('ip', '-'):16} {info.get('device', '-')}")
    elif args.what == "info":
        data = client_request(server, token, "GET", f"/api/devices/{args.device}")
        print(json.dumps(data, indent=2, ensure_ascii=False))
    elif args.what == "call":
        params = dict(kv.split("=", 1) for kv in args.params)
        data = client_request(server, token, "POST",
                              f"/api/devices/{args.device}/cmd/{args.method}",
                              params)
        print(json.dumps(data, indent=2, ensure_ascii=False))
    elif args.what == "wol":
        data = client_request(server, token, "POST",
                              f"/api/devices/{args.device}/cmd/net/wol",
                              {"mac": args.mac})
        print(json.dumps(data))
    elif args.what == "ping":
        data = client_request(server, token, "POST",
                              f"/api/devices/{args.device}/cmd/net/ping",
                              {"host": args.host, "count": args.count})
        print(json.dumps(data))


# ---------------------------------------------------------------- main

def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)   # unbuffered service logs
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="AT-Node remote broker")
    sub = ap.add_subparsers(dest="mode", required=True)

    sp = sub.add_parser("serve", help="run MQTT broker (+ optional HTTP proxy)")
    sp.add_argument("--http", nargs="?", const=8080, default=None, type=int,
                    metavar="PORT",
                    help="also start the HTTP proxy (default port 8080); "
                         "omit to run MQTT broker only")
    sp.add_argument("--mqtt-port", type=int, default=1883)
    sp.add_argument("--mqtt-tls-port", type=int, default=8883)
    sp.add_argument("--certs", default=os.path.join(os.path.dirname(__file__), "certs"),
                    help="dir with server.crt/server.key for MQTT TLS")
    sp.add_argument("--token", default=None, help="override HTTP bearer token")

    cp = sub.add_parser("client", help="query a running broker")
    cp.add_argument("what", choices=["list", "info", "call", "wol", "ping"])
    cp.add_argument("device", nargs="?")
    cp.add_argument("method", nargs="?")
    cp.add_argument("params", nargs="*")
    cp.add_argument("--server", default=None)
    cp.add_argument("--token", default=None)
    cp.add_argument("--mac", default=None)
    cp.add_argument("--host", default=None)
    cp.add_argument("--count", default="4")

    args = ap.parse_args()

    if args.mode == "client":
        # positional mapping: wol <device> <mac> / ping <device> <host> [count]
        # (second positional lands in 'method', third in params[0])
        if args.what == "wol":
            args.mac = args.method
        if args.what == "ping":
            args.host = args.method
            if args.params:
                args.count = args.params[0]
        if args.what in ("info", "call", "wol", "ping") and not args.device:
            ap.error(f"client {args.what} requires <device>")
        if args.what == "call" and not args.method:
            ap.error("client call requires <method>")
        if args.what == "wol" and not args.mac:
            ap.error("client wol requires <device> <mac>")
        if args.what == "ping" and not args.host:
            ap.error("client ping requires <device> <host>")
        return run_client(args)

    # serve
    cfg = load_or_create_config(args)
    print(f"[cfg] {CONFIG_PATH}")
    print(f"[cfg] MQTT credentials  : {cfg['mqtt_user']} / {cfg['mqtt_password']}")

    loop = asyncio.new_event_loop()
    t = threading.Thread(target=start_amqtt,
                         args=(cfg, args.mqtt_port, args.mqtt_tls_port,
                               args.certs, loop), daemon=True)
    t.start()
    time.sleep(1.5)   # let the broker come up

    if args.http is None:
        print("[http] proxy disabled (start it with --http [PORT])")
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            pass
        return

    print(f"[cfg] HTTP bearer token : {cfg['token']}")
    bridge = Bridge(cfg, args.mqtt_port)

    httpd = ThreadingHTTPServer(("0.0.0.0", args.http), Handler)
    httpd.cfg = cfg
    httpd.bridge = bridge
    print(f"[http] proxy listening: 0.0.0.0:{args.http} "
          f"(GET /api/help for docs)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

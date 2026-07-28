#!/usr/bin/env python3
"""atnode_broker.py — single-file remote broker for ESP32 AT-Node devices.

Architecture (one file, layered sections, ~900 lines):

  ┌─ serve ────────────────────────────────────────────────────────┐
  │  BrokerService   amqtt broker :1883/:8883(TLS)                 │
  │    ├ ApiKeyAuthPlugin   SQLite key auth; localhost bypass      │
  │    ├ ManageAclPlugin    $manage/# topics restricted to local   │
  │    └ AccessLog          per-key files (~/.atnode_broker_logs/) │
  │  ManageService   $manage/cmd endpoint (key CRUD over MQTT)     │
  │  Bridge+HttpProxy (OPT-IN --http)  curl facade                 │
  ├─ client  ── pure MQTT client role (device control, any broker) │
  └─ manager ─ key admin (MQTT) + cert tools (local, no MQTT) ────┘

MQTT wire protocol:
  atnode/<id>/state   retained "online"/"offline" (LWT)
  atnode/<id>/info    retained JSON manifest (services catalog)
  atnode/<id>/cmd     "<reqid> <method> <urlencoded query>"
  atnode/<id>/resp    {"id":..,"ok":.., ...}
  $manage/cmd|resp    key management (localhost only)

Auth model:
  - Remote MQTT clients: username = API key (must exist and be 'active'
    in the SQLite key DB). Localhost clients: no key required.
  - HTTP proxy: Bearer <api-key>; localhost requests bypass.
  - manager talks MQTT to localhost (use SSH port-forward from outside).

Usage:
  atnode_broker.py serve [--mqtt-port 1883] [--mqtt-tls-port 8883]
                         [--certs DIR] [--http [PORT]]
  atnode_broker.py client list|info|call|wol|ping ... [--server H] [--key K]
  atnode_broker.py manager key add --name alice
  atnode_broker.py manager key list|revoke|enable|remove ...
  atnode_broker.py manager certs gen [--ip IP] [--certs DIR]
  atnode_broker.py manager certs fingerprint|info|verify [--certs DIR]
"""

import argparse
import asyncio
import datetime
import json
import os
import re
import secrets
import sqlite3
import ssl
import sys
import threading
import time
import urllib.parse
import uuid
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ------------------------------------------------------------------
# Section 0: paths & constants
# ------------------------------------------------------------------

CONFIG_PATH = os.environ.get("ATNODE_BROKER_CONFIG",
                             os.path.expanduser("~/.atnode_broker.json"))
KEYS_DB = os.environ.get("ATNODE_KEYS_DB",
                         os.path.expanduser("~/.atnode_broker_keys.sqlite"))
LOG_DIR = os.environ.get("ATNODE_LOG_DIR",
                         os.path.expanduser("~/.atnode_broker_logs"))

LOCAL_HOSTS = ("127.0.0.1", "::1", "localhost")
MANAGE_CMD_TOPIC = "_manage/cmd"
MANAGE_RESP_TOPIC = "_manage/resp"

# ==================================================================
# Section 1: KeyStore — SQLite API key database
# ==================================================================

class KeyStore:
    """API key database. Each key has a name and a status
    (active / revoked). Safe to open per-operation (SQLite)."""

    SCHEMA = """
    CREATE TABLE IF NOT EXISTS api_keys (
        key        TEXT PRIMARY KEY,
        name       TEXT NOT NULL DEFAULT '',
        status     TEXT NOT NULL DEFAULT 'active',   -- active | revoked
        created_at TEXT NOT NULL,
        revoked_at TEXT
    );
    """

    def __init__(self, path=KEYS_DB):
        self.path = path
        with self._conn() as c:
            c.execute(self.SCHEMA)

    def _conn(self):
        return sqlite3.connect(self.path)

    @staticmethod
    def _row(r):
        return {"key": r[0], "name": r[1], "status": r[2],
                "created_at": r[3], "revoked_at": r[4]}

    def add(self, name, key=None):
        key = key or secrets.token_hex(16)
        with self._conn() as c:
            c.execute(
                "INSERT INTO api_keys(key,name,status,created_at) "
                "VALUES(?,?, 'active', ?)",
                (key, name, datetime.datetime.now().isoformat(timespec="seconds")))
        return key

    def get(self, key):
        with self._conn() as c:
            r = c.execute("SELECT * FROM api_keys WHERE key=?", (key,)).fetchone()
        return self._row(r) if r else None

    def find(self, key_or_name):
        """Resolve by exact key, or by unique name."""
        rec = self.get(key_or_name)
        if rec:
            return rec
        with self._conn() as c:
            rows = c.execute("SELECT * FROM api_keys WHERE name=?",
                             (key_or_name,)).fetchall()
        return self._row(rows[0]) if len(rows) == 1 else None

    def set_status(self, key_or_name, status):
        rec = self.find(key_or_name)
        if not rec:
            return None
        revoked_at = (datetime.datetime.now().isoformat(timespec="seconds")
                      if status == "revoked" else None)
        with self._conn() as c:
            c.execute("UPDATE api_keys SET status=?, revoked_at=? WHERE key=?",
                      (status, revoked_at, rec["key"]))
        rec["status"] = status
        rec["revoked_at"] = revoked_at
        return rec

    def remove(self, key_or_name):
        rec = self.find(key_or_name)
        if not rec:
            return None
        with self._conn() as c:
            c.execute("DELETE FROM api_keys WHERE key=?", (rec["key"],))
        return rec

    def list(self):
        with self._conn() as c:
            rows = c.execute("SELECT * FROM api_keys ORDER BY created_at").fetchall()
        return [self._row(r) for r in rows]

    def valid(self, key):
        """Key exists AND is active."""
        rec = self.get(key) if key else None
        return rec if (rec and rec["status"] == "active") else None


# ==================================================================
# Section 2: AccessLog — per-key local log files (no DB)
# ==================================================================

class AccessLog:
    """Append-only access logs, one file per key name:
      ~/.atnode_broker_logs/<name>.log      successful access per key
      ~/.atnode_broker_logs/auth_fail.log   rejected attempts
      ~/.atnode_broker_logs/local.log       localhost (unauthenticated) access
    Line format: <iso-time> event=<e> key=<name> <k=v ...>
    """

    def __init__(self, log_dir=LOG_DIR):
        self.dir = log_dir
        os.makedirs(self.dir, exist_ok=True)
        self._lock = threading.Lock()

    def write(self, key_name, event, **fields):
        ts = datetime.datetime.now().isoformat(timespec="seconds")
        parts = " ".join(f"{k}={v}" for k, v in fields.items())
        line = f"{ts} event={event} key={key_name or '-'} {parts}\n"
        safe = re.sub(r"[^A-Za-z0-9_.-]", "_", key_name or "")
        fname = {"": "auth_fail"}.get(safe, safe)
        with self._lock:
            with open(os.path.join(self.dir, fname + ".log"), "a",
                      encoding="utf-8") as f:
                f.write(line)


# ==================================================================
# Section 3: amqtt plugins — key auth + manage-topic ACL
# ==================================================================

try:
    from amqtt.plugins.base import BaseAuthPlugin, BaseTopicPlugin  # noqa: E402
except ModuleNotFoundError:
    # client/manager/deploy modes don't need amqtt; provide stubs for class defs
    class BaseAuthPlugin:  # type: ignore[no-redef]
        pass
    class BaseTopicPlugin:  # type: ignore[no-redef]
        pass


class ApiKeyAuthPlugin(BaseAuthPlugin):
    """MQTT CONNECT authentication:
      - localhost sessions: allowed (no key), logged as 'local'
      - remote sessions: username must be an ACTIVE api key
    Every decision is written to the access log."""

    @dataclass
    class Config:
        db_path: str = KEYS_DB
        log_dir: str = LOG_DIR

    def __init__(self, context):
        super().__init__(context)
        self._db = self._get_config_option("db_path", KEYS_DB)
        self._log_dir = self._get_config_option("log_dir", LOG_DIR)

    async def authenticate(self, *, session):
        base = await super().authenticate(session=session)
        if base is False:
            return False
        host = session.remote_address or ""
        cid = session.client_id or ""
        log = AccessLog(self._log_dir)
        if host in LOCAL_HOSTS:
            log.write("local", "mqtt_connect", client_id=cid, ip=host)
            return True
        rec = KeyStore(self._db).valid(session.username or "")
        if rec:
            log.write(rec["name"], "mqtt_connect", client_id=cid, ip=host)
            return True
        log.write("", "mqtt_connect_deny", client_id=cid, ip=host,
                  tried_key=(session.username or "")[:8])
        return False


class ManageAclPlugin(BaseTopicPlugin):
    """$manage/# topics (key administration) are localhost-only."""

    async def topic_filtering(self, *, session=None, topic=None, action=None):
        if topic and topic.startswith("_manage/"):
            host = (session.remote_address if session else "") or ""
            return host in LOCAL_HOSTS
        return True


# ==================================================================
# Section 4: BrokerService — embedded amqtt broker (thread)
# ==================================================================

class BrokerService:
    def __init__(self, mqtt_port, mqtt_tls_port, certs_dir):
        self.mqtt_port = mqtt_port
        self.mqtt_tls_port = mqtt_tls_port
        self.certs_dir = certs_dir
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.thread.start()

    def _config(self):
        listeners = {"default": {"type": "tcp", "bind": f"0.0.0.0:{self.mqtt_port}"}}
        cert = key = None
        if self.certs_dir:
            cert = os.path.join(self.certs_dir, "server.crt")
            key = os.path.join(self.certs_dir, "server.key")
        if cert and key and os.path.exists(cert) and os.path.exists(key):
            listeners["tls"] = {
                "type": "tcp", "bind": f"0.0.0.0:{self.mqtt_tls_port}",
                "ssl": True, "certfile": cert, "keyfile": key,
            }
        return {
            "listeners": listeners,
            "plugins": {
                "amqtt.plugins.authentication.AnonymousAuthPlugin": {
                    "allow_anonymous": True,     # real gating in ApiKeyAuthPlugin
                },
                "__main__.ApiKeyAuthPlugin": {
                    "db_path": KEYS_DB, "log_dir": LOG_DIR,
                },
                "__main__.ManageAclPlugin": {},
                "amqtt.plugins.sys.broker.BrokerSysPlugin": {"sys_interval": 20},
            },
        }

    def _run(self):
        from amqtt.broker import Broker

        config = self._config()

        async def run():
            broker = Broker(config)
            await broker.start()
            binds = [l["bind"] for l in config["listeners"].values()]
            print(f"[mqtt] broker listening: {', '.join(binds)} "
                  f"(key auth: {KEYS_DB})", flush=True)
            await asyncio.Event().wait()

        self.loop.run_until_complete(run())


# ==================================================================
# Section 5: Bridge — MQTT client: device registry + RPC correlation
# ==================================================================

class Bridge:
    """Used by the HTTP proxy (localhost) and the client role (any broker)."""

    def __init__(self, host="127.0.0.1", port=1883, ca=None, key=None,
                 client_id="atnode-broker-bridge"):
        import paho.mqtt.client as mqtt

        self.devices = {}            # id -> {"online","info","last_seen"}
        self.pending = {}            # reqid -> {"event","resp"}
        self.lock = threading.Lock()
        self.connected = threading.Event()

        self.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                client_id=client_id)
        if key:
            self.mqtt.username_pw_set(key, key)
        else:   # username only so anonymous-plugin passes; auth bypass is by IP
            self.mqtt.username_pw_set("local", None)
        self.mqtt.on_connect = self._on_connect
        self.mqtt.on_message = self._on_message
        if ca or port == 8883:
            have_ca = ca and os.path.exists(ca)
            self.mqtt.tls_set(ca_certs=ca if have_ca else None,
                              cert_reqs=ssl.CERT_REQUIRED if have_ca else ssl.CERT_NONE)
            # Skip hostname check: self-signed certs use IP SAN, tunnels change host
            self.mqtt.tls_insecure_set(True)
        self.mqtt.connect(host, port, keepalive=30)
        self.mqtt.loop_start()
        self.connected.wait(5)

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        client.subscribe("atnode/+/state")
        client.subscribe("atnode/+/info")
        client.subscribe("atnode/+/resp")
        self.connected.set()
        print("[bridge] connected to broker", flush=True)

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
                p = self.pending.get(data.get("id"))
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


# ==================================================================
# Section 6: ManageService — $manage endpoint (key CRUD over MQTT)
# ==================================================================

class ManageService:
    """Always-on localhost MQTT endpoint inside serve:
    receives "$manage/cmd" -> KeyStore op -> "$manage/resp".
    Reachable only from localhost (ManageAclPlugin)."""

    OPS = ("add", "revoke", "enable", "remove", "list")

    def __init__(self, port=1883):
        import paho.mqtt.client as mqtt

        self.ks = KeyStore()
        self.log = AccessLog()
        self.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                client_id="atnode-manage-service")
        self.mqtt.username_pw_set("local", None)
        self.mqtt.on_connect = lambda c, u, f, rc, p: c.subscribe(MANAGE_CMD_TOPIC)
        self.mqtt.on_message = self._on_message
        self.mqtt.connect("127.0.0.1", port, keepalive=30)
        self.mqtt.loop_start()

    def _exec(self, op, arg):
        if op == "add":
            if not arg:
                return {"ok": False, "error": "usage: add <name>"}
            key = self.ks.add(arg)
            self.log.write(arg, "key_add")
            return {"ok": True, "key": key, "name": arg}
        if op == "list":
            return {"ok": True, "keys": self.ks.list()}
        if op in ("revoke", "enable"):
            if not arg:
                return {"ok": False, "error": f"usage: {op} <key|name>"}
            rec = self.ks.set_status(arg, "revoked" if op == "revoke" else "active")
            if not rec:
                return {"ok": False, "error": "key not found (or ambiguous name)"}
            self.log.write(rec["name"], f"key_{op}")
            return {"ok": True, "key": rec["key"], "name": rec["name"],
                    "status": rec["status"]}
        if op == "remove":
            if not arg:
                return {"ok": False, "error": "usage: remove <key|name>"}
            rec = self.ks.remove(arg)
            if not rec:
                return {"ok": False, "error": "key not found (or ambiguous name)"}
            self.log.write(rec["name"], "key_remove")
            return {"ok": True, "removed": rec}
        return {"ok": False, "error": f"unknown op {op!r}"}

    def _on_message(self, client, userdata, msg):
        body = msg.payload.decode("utf-8", "replace")
        parts = body.split(" ", 2)
        if len(parts) < 2:
            return
        reqid, op = parts[0], parts[1]
        arg = parts[2].strip() if len(parts) > 2 else ""
        resp = self._exec(op, arg)
        resp["id"] = reqid
        client.publish(MANAGE_RESP_TOPIC, json.dumps(resp))


class ManageClient:
    """manager-role MQTT client (localhost or SSH-forwarded)."""

    def __init__(self, host="127.0.0.1", port=1883):
        import paho.mqtt.client as mqtt

        self._resp = None
        self._event = threading.Event()
        self.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                client_id=f"atnode-manager-{uuid.uuid4().hex[:6]}")
        self.mqtt.username_pw_set("local", None)
        self.mqtt.on_connect = lambda c, u, f, rc, p: (
            c.subscribe(MANAGE_RESP_TOPIC), self._event.set())
        self.mqtt.on_message = self._on_message
        self.mqtt.connect(host, port, keepalive=15)
        self.mqtt.loop_start()
        self._event.wait(5)
        self._event.clear()

    def _on_message(self, client, userdata, msg):
        try:
            self._resp = json.loads(msg.payload.decode())
        except json.JSONDecodeError:
            self._resp = {"ok": False, "error": "bad response"}
        self._event.set()

    def exec(self, op, arg="", timeout=5):
        reqid = uuid.uuid4().hex[:8]
        self.mqtt.publish(MANAGE_CMD_TOPIC, f"{reqid} {op} {arg}".strip())
        if self._event.wait(timeout):
            self._event.clear()
            return self._resp
        return {"ok": False, "error": "manage endpoint timeout "
                "(is 'serve' running on this host?)"}

    def close(self):
        self.mqtt.loop_stop()
        self.mqtt.disconnect()


# ==================================================================
# Section 7: HttpProxy — curl facade (OPT-IN, Bearer = api key)
# ==================================================================

HELP_TEXT = """AT-Node remote broker API

Auth: header  Authorization: Bearer <api-key>   (localhost bypasses)

  GET  /api/help                          this text (no auth)
  GET  /api/devices                       list devices + online state
  GET  /api/devices/<id>                  device detail + services + usage
  POST /api/devices/<id>/cmd/<method>     run a device method
       params via query string, form body, or JSON body

Common methods (see /api/devices/<id> for the live catalog):
  keyboard/tap mods,k,ms | keyboard/text s,ms,gap | keyboard/key mods,k0..k5
  gpio/write pin,level   | gpio/read pin          | adc/read ch
  ble/status             | net/wol mac            | net/ping host,count
  sys/info

Example:
  curl -H "Authorization: Bearer $KEY" \\
    -X POST "http://server:8080/api/devices/<id>/cmd/keyboard/text?s=Hello"
"""


class HttpHandler(BaseHTTPRequestHandler):
    server_version = "ATNodeBroker/2.0"

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

    def _client_ip(self):
        return self.client_address[0]

    def _auth_name(self):
        """Returns key name if authorized, else None. Localhost bypasses."""
        if self._client_ip() in LOCAL_HOSTS:
            return "local"
        h = self.headers.get("Authorization", "")
        if h.startswith("Bearer "):
            rec = KeyStore().valid(h[7:].strip())
            if rec:
                return rec["name"]
        return None

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
        name = self._auth_name()
        if not name:
            AccessLog().write("", "http_deny", path=url.path, ip=self._client_ip())
            return self._send(401, {"ok": False, "error": "unauthorized"})
        if url.path == "/api/devices":
            with self.server.bridge.lock:
                devs = [{"id": k,
                         "online": v.get("online", False),
                         "info": v.get("info"),
                         "last_seen": v.get("last_seen")}
                        for k, v in sorted(self.server.bridge.devices.items())]
            AccessLog().write(name, "http", path=url.path, ip=self._client_ip())
            return self._send(200, {"devices": devs})
        if url.path.startswith("/api/devices/"):
            dev = url.path.split("/")[3]
            with self.server.bridge.lock:
                d = self.server.bridge.devices.get(dev)
            if not d:
                return self._send(404, {"ok": False, "error": "unknown device"})
            info = d.get("info") or {}
            AccessLog().write(name, "http", path=url.path, ip=self._client_ip())
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
        name = self._auth_name()
        if not name:
            AccessLog().write("", "http_deny", path=url.path, ip=self._client_ip())
            return self._send(401, {"ok": False, "error": "unauthorized"})
        parts = url.path.strip("/").split("/")
        if len(parts) >= 5 and parts[0] == "api" and parts[1] == "devices" \
                and parts[3] == "cmd":
            dev, method = parts[2], "/".join(parts[4:])
            params = self._params(url)
            AccessLog().write(name, "http_cmd", device=dev, method=method,
                              ip=self._client_ip())
            return self._send(200, self.server.bridge.rpc(dev, method, params))
        return self._send(404, {"ok": False, "error": "not found"})


# ==================================================================
# Section 8: roles — serve / client / manager
# ==================================================================

def role_serve(args):
    cfg = {}
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg = json.load(f)

    KeyStore()    # ensure DB exists
    BrokerService(args.mqtt_port, args.mqtt_tls_port, args.certs).start()
    time.sleep(1.5)

    manage = ManageService(port=args.mqtt_port)
    print("[manage] $manage endpoint ready (localhost only)", flush=True)

    if args.http is None:
        print("[http] proxy disabled (start it with --http [PORT])", flush=True)
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            pass
        return

    bridge = Bridge(port=args.mqtt_port)
    httpd = ThreadingHTTPServer(("0.0.0.0", args.http), HttpHandler)
    httpd.bridge = bridge
    print(f"[http] proxy listening: 0.0.0.0:{args.http} "
          f"(GET /api/help for docs)", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


_CLIENT_CFG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "client.toml")


def _load_client_config():
    """Load client.toml if present. Returns dict with server/port/ca/key."""
    if not os.path.exists(_CLIENT_CFG):
        return {}
    try:
        try:
            import tomllib
        except ModuleNotFoundError:
            import tomli as tomllib  # Python < 3.11 fallback
        with open(_CLIENT_CFG, "rb") as f:
            data = tomllib.load(f)
        return data.get("client", data)  # allow flat or [client] section
    except Exception as e:
        print(f"warn: failed to parse {_CLIENT_CFG}: {e}", file=sys.stderr)
        return {}


def role_client(args):
    cfg = _load_client_config()
    host = args.server or cfg.get("server", "127.0.0.1")
    port = args.port or int(cfg.get("port", 1883))
    ca = args.ca or cfg.get("ca", None)
    # resolve relative ca path against config file dir
    if ca and not os.path.isabs(ca):
        ca = os.path.join(os.path.dirname(_CLIENT_CFG), ca)
    cred_key = args.key or os.environ.get("ATNODE_KEY", "") or cfg.get("key", "")
    bridge = Bridge(host=host, port=port, ca=ca,
                    key=cred_key or None, client_id="atnode-client")
    if not bridge.connected.is_set():
        print(f"error: cannot connect to {host}:{port}",
              file=sys.stderr)
        sys.exit(2)

    try:
        if args.what == "list":
            time.sleep(1.5)
            with bridge.lock:
                devs = sorted(bridge.devices.items())
            if not devs:
                print("no devices seen yet")
                return
            print(f"{'ID':24} {'STATE':8} {'IP':16} NAME")
            for dev, d in devs:
                info = d.get("info") or {}
                print(f"{dev:24} "
                      f"{'online' if d.get('online') else 'offline':8} "
                      f"{info.get('ip', '-'):16} {info.get('device', '-')}")
        elif args.what == "info":
            time.sleep(1.5)
            with bridge.lock:
                d = bridge.devices.get(args.device)
            if not d:
                print(f"unknown device: {args.device}")
                return
            info = d.get("info") or {}
            print(json.dumps({
                "id": args.device,
                "online": d.get("online", False),
                "info": info,
                "services": info.get("services", []),
                "usage": f"client call {args.device} <method> k=v ...",
            }, indent=2, ensure_ascii=False))
        elif args.what == "call":
            params = dict(kv.split("=", 1) for kv in args.params)
            print(json.dumps(bridge.rpc(args.device, args.method, params),
                             indent=2, ensure_ascii=False))
        elif args.what == "wol":
            print(json.dumps(bridge.rpc(args.device, "net/wol",
                                        {"mac": args.mac})))
        elif args.what == "ping":
            print(json.dumps(bridge.rpc(args.device, "net/ping",
                                        {"host": args.host,
                                         "count": args.count})))
    finally:
        bridge.mqtt.loop_stop()
        bridge.mqtt.disconnect()


def role_manager_key(args):
    mc = ManageClient(host=args.server, port=args.port)
    try:
        if args.op == "add":
            if not args.name:
                print("error: manager add requires --name", file=sys.stderr)
                sys.exit(2)
            resp = mc.exec("add", args.name)
            if resp.get("ok"):
                print(f"key created for {resp['name']!r}:\n  {resp['key']}")
                print("store it now - pass it to clients via --key / $ATNODE_KEY")
            else:
                print(json.dumps(resp))
        elif args.op == "list":
            resp = mc.exec("list")
            keys = resp.get("keys", [])
            if not keys:
                print("no keys (add one: manager add --name <n>)")
                return
            print(f"{'KEY':34} {'NAME':16} {'STATUS':8} CREATED")
            for k in keys:
                print(f"{k['key']:34} {k['name']:16} {k['status']:8} {k['created_at']}")
        else:
            target = args.target
            if not target:
                print(f"error: manager {args.op} requires <key|name>",
                      file=sys.stderr)
                sys.exit(2)
            print(json.dumps(mc.exec(args.op, target), ensure_ascii=False))
    finally:
        mc.close()


def role_manager_certs(args):
    """Local certificate management (no MQTT needed)."""
    import hashlib
    import subprocess

    certs_dir = args.certs
    os.makedirs(certs_dir, exist_ok=True)
    ca_crt = os.path.join(certs_dir, "ca.crt")
    ca_key = os.path.join(certs_dir, "ca.key")
    srv_crt = os.path.join(certs_dir, "server.crt")
    srv_key = os.path.join(certs_dir, "server.key")
    srv_csr = os.path.join(certs_dir, "server.csr")

    def run(cmd, **kw):
        r = subprocess.run(cmd, capture_output=True, text=True, **kw)
        if r.returncode != 0:
            print(f"error: {' '.join(cmd)}\n{r.stderr.strip()}", file=sys.stderr)
            sys.exit(1)
        return r.stdout.strip()

    if args.op == "gen":
        ip = args.ip
        if not ip:
            ip = input("Server public IP (for SAN): ").strip()
        if not ip:
            print("error: --ip required", file=sys.stderr)
            sys.exit(2)
        days = str(args.days)
        # 1. CA
        run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", ca_key, "-out", ca_crt, "-days", days,
             "-subj", "/CN=AT-Node-CA"])
        # 2. Server key + CSR
        run(["openssl", "req", "-newkey", "rsa:2048", "-nodes",
             "-keyout", srv_key, "-out", srv_csr,
             "-subj", f"/CN={ip}"])
        # 3. Sign with SAN
        import tempfile
        ext = f"subjectAltName=IP:{ip}"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".cnf",
                                         delete=False) as tf:
            tf.write(ext)
            ext_file = tf.name
        try:
            run(["openssl", "x509", "-req", "-in", srv_csr,
                 "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
                 "-out", srv_crt, "-days", days,
                 "-extfile", ext_file])
        finally:
            os.remove(ext_file)
        os.remove(srv_csr)
        print(f"[ok] certs generated in {certs_dir}")
        print(f"     CA:     {ca_crt}")
        print(f"     Server: {srv_crt} (SAN IP:{ip}, {days} days)")
        # auto-show fingerprint
        _print_fingerprint(srv_crt)

    elif args.op == "fingerprint":
        if not os.path.exists(srv_crt):
            print(f"error: {srv_crt} not found (run: manager certs gen)",
                  file=sys.stderr)
            sys.exit(1)
        _print_fingerprint(srv_crt)

    elif args.op == "info":
        target = srv_crt if os.path.exists(srv_crt) else ca_crt
        if not os.path.exists(target):
            print(f"error: no certs in {certs_dir}", file=sys.stderr)
            sys.exit(1)
        out = run(["openssl", "x509", "-in", target, "-noout",
                   "-subject", "-issuer", "-dates", "-ext",
                   "subjectAltName"])
        print(out)

    elif args.op == "verify":
        if not os.path.exists(ca_crt) or not os.path.exists(srv_crt):
            print("error: need both ca.crt and server.crt", file=sys.stderr)
            sys.exit(1)
        out = run(["openssl", "verify", "-CAfile", ca_crt, srv_crt])
        print(out)


def _print_fingerprint(cert_path):
    """Print SHA256 fingerprint of a DER-encoded cert."""
    import hashlib
    import subprocess
    der = subprocess.run(
        ["openssl", "x509", "-in", cert_path, "-outform", "DER"],
        capture_output=True)
    if der.returncode != 0:
        print("error: cannot parse cert", file=sys.stderr)
        return
    fp = hashlib.sha256(der.stdout).hexdigest()
    pairs = ":".join(fp[i:i+2].upper() for i in range(0, len(fp), 2))
    print(f"SHA256 fingerprint ({os.path.basename(cert_path)}):")
    print(f"  {pairs}")


# ==================================================================
# Section 7: Deploy — systemd user service management
# ==================================================================

_UNIT_TEMPLATE = """\
[Unit]
Description=AT-Node MQTT Broker
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory={work_dir}
ExecStart={python} {script} serve --certs {certs} --mqtt-port {mqtt_port} --mqtt-tls-port {tls_port}{http_flag}
Restart=on-failure
RestartSec=5
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=default.target
"""


def _systemctl(*cmd, check=True):
    """Run systemctl --user with given args."""
    import subprocess
    full = ["systemctl", "--user"] + list(cmd)
    return subprocess.run(full, check=check)


def role_deploy(args):
    """Manage the broker as a systemd user service."""
    import subprocess

    if sys.platform == "win32":
        print("ERROR: deploy requires Linux systemd. Run on the server.")
        return 1

    op = args.op
    svc = args.name

    # --- simple pass-through operations ---
    if op in ("start", "stop", "restart", "enable", "disable"):
        _systemctl(op, f"{svc}.service")
        if op == "restart":
            print(f"[deploy] {svc} restarted.")
        return 0

    if op == "status":
        _systemctl("status", f"{svc}.service", check=False)
        # show fingerprint as bonus info
        certs_dir = args.certs
        crt = os.path.join(certs_dir, "server.crt")
        if os.path.exists(crt):
            print()
            _print_fingerprint(crt)
        return 0

    if op == "logs":
        os.execvp("journalctl", ["journalctl", "--user", "-u", f"{svc}.service", "-f"])
        return 0  # unreachable

    if op == "uninstall":
        _systemctl("stop", f"{svc}.service", check=False)
        _systemctl("disable", f"{svc}.service", check=False)
        unit_path = os.path.expanduser(f"~/.config/systemd/user/{svc}.service")
        if os.path.exists(unit_path):
            os.remove(unit_path)
            print(f"[deploy] removed {unit_path}")
        _systemctl("daemon-reload", check=False)
        print(f"[deploy] {svc} uninstalled.")
        return 0

    # --- install ---
    assert op == "install"
    work_dir = os.path.dirname(os.path.abspath(__file__))
    certs_dir = os.path.abspath(args.certs)

    # 1. certificate handling
    if args.gen_certs:
        if not args.ip:
            print("ERROR: --gen-certs requires --ip <server-public-IP>")
            return 1
        print(f"[deploy] generating certificates (SAN={args.ip}) ...")

        class _CertsArgs:
            pass

        ca = _CertsArgs()
        ca.op = "gen"
        ca.certs = certs_dir
        ca.ip = args.ip
        ca.days = 3650
        role_manager_certs(ca)
    else:
        crt = os.path.join(certs_dir, "server.crt")
        key = os.path.join(certs_dir, "server.key")
        if not os.path.exists(crt) or not os.path.exists(key):
            print(f"ERROR: certs not found in {certs_dir}")
            print("  Use --gen-certs --ip <IP> to generate, or --certs <DIR>.")
            return 1

    # 2. build unit content
    python = sys.executable
    script = os.path.join(work_dir, "atnode_broker.py")
    http_flag = f" --http {args.http_port}" if args.http else ""
    unit_content = _UNIT_TEMPLATE.format(
        work_dir=work_dir,
        python=python,
        script=script,
        certs=certs_dir,
        mqtt_port=args.mqtt_port,
        tls_port=args.mqtt_tls_port,
        http_flag=http_flag,
    )

    # 3. write unit file
    unit_dir = os.path.expanduser("~/.config/systemd/user")
    os.makedirs(unit_dir, exist_ok=True)
    unit_path = os.path.join(unit_dir, f"{svc}.service")
    with open(unit_path, "w") as f:
        f.write(unit_content)
    print(f"[deploy] wrote {unit_path}")

    # 4. enable linger (service survives logout / starts on boot)
    user = os.environ.get("USER", os.environ.get("LOGNAME", ""))
    if user:
        r = subprocess.run(["loginctl", "enable-linger", user],
                           check=False, capture_output=True, text=True)
        if r.returncode != 0:
            print("[deploy] WARN: enable-linger failed; run as root:")
            print(f"         sudo loginctl enable-linger {user}")
            if r.stderr:
                print(f"         ({r.stderr.strip()})")

    # 5. reload + enable + start
    _systemctl("daemon-reload")
    _systemctl("enable", f"{svc}.service")
    _systemctl("restart", f"{svc}.service")
    print(f"[deploy] {svc} installed and started.")

    # 6. show status
    import time
    time.sleep(1)
    _systemctl("status", f"{svc}.service", check=False)
    return 0


# ==================================================================
# main
# ==================================================================

def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="AT-Node remote broker")
    sub = ap.add_subparsers(dest="mode", required=True)

    sp = sub.add_parser("serve", help="run MQTT broker (+ optional HTTP proxy)")
    sp.add_argument("--http", nargs="?", const=8080, default=None, type=int,
                    metavar="PORT",
                    help="also start the HTTP proxy (default port 8080)")
    sp.add_argument("--mqtt-port", type=int, default=1883)
    sp.add_argument("--mqtt-tls-port", type=int, default=8883)
    sp.add_argument("--certs", default=os.path.join(os.path.dirname(__file__), "certs"))

    cp = sub.add_parser("client", help="device control (MQTT client role)")
    cp.add_argument("what", choices=["list", "info", "call", "wol", "ping"])
    cp.add_argument("device", nargs="?")
    cp.add_argument("method", nargs="?")
    cp.add_argument("params", nargs="*")
    cp.add_argument("--server", default=None, help="broker host (or client.toml)")
    cp.add_argument("--port", type=int, default=None, help="broker port (or client.toml)")
    cp.add_argument("--ca", default=None, help="CA cert for TLS (port 8883)")
    cp.add_argument("--key", default=None, help="API key (or $ATNODE_KEY)")
    cp.add_argument("--count", default="4")

    mp = sub.add_parser("manager", help="admin: key management + cert tools")
    msub = mp.add_subparsers(dest="mgr_mode", required=True)

    mk = msub.add_parser("key", help="API key admin (MQTT, localhost/SSH)")
    mk.add_argument("op", choices=["add", "list", "revoke", "enable", "remove"])
    mk.add_argument("target", nargs="?", help="key or name (revoke/enable/remove)")
    mk.add_argument("--name", default=None, help="name for 'add'")
    mk.add_argument("--server", default="127.0.0.1")
    mk.add_argument("--port", type=int, default=1883)

    mc = msub.add_parser("certs", help="TLS certificate tools (local, no MQTT)")
    mc.add_argument("op", choices=["gen", "fingerprint", "info", "verify"])
    mc.add_argument("--certs", default=os.path.join(os.path.dirname(__file__), "certs"),
                    help="certs directory")
    mc.add_argument("--ip", default=None, help="server IP for SAN (gen)")
    mc.add_argument("--days", type=int, default=3650, help="cert validity days (gen)")

    dp = sub.add_parser("deploy", help="systemd user service management (Linux)")
    dp.add_argument("op", choices=["install", "start", "stop", "restart",
                                   "status", "enable", "disable", "logs", "uninstall"])
    dp.add_argument("--mqtt-port", type=int, default=1883)
    dp.add_argument("--mqtt-tls-port", type=int, default=8883)
    dp.add_argument("--http", action="store_true",
                    help="enable HTTP proxy (INSECURE, off by default)")
    dp.add_argument("--http-port", type=int, default=8080)
    dp.add_argument("--certs", default=os.path.join(os.path.dirname(__file__), "certs"))
    dp.add_argument("--gen-certs", action="store_true",
                    help="generate CA + server cert (requires --ip)")
    dp.add_argument("--ip", default=None, help="server public IP for cert SAN")
    dp.add_argument("--name", default="atnode-broker", help="service name")

    args = ap.parse_args()

    if args.mode == "client":
        # positional mapping: wol <device> <mac> / ping <device> <host> [count]
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
        return role_client(args)

    if args.mode == "manager":
        if args.mgr_mode == "key":
            return role_manager_key(args)
        else:
            return role_manager_certs(args)

    if args.mode == "deploy":
        return role_deploy(args)

    return role_serve(args)


if __name__ == "__main__":
    main()

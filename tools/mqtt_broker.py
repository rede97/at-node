#!/usr/bin/env python3
r"""Local MQTT broker for ESP32 AT Node testing.

  Uses amqtt to run a lightweight broker on localhost:1883 (plain) and
  localhost:8883 (TLS, if certs are present in tools/certs/).

  No authentication — suitable for local development only.

  Usage:
      uv run python tools/mqtt_broker.py
"""
import asyncio
import logging
import os

from amqtt.broker import Broker

logging.basicConfig(level=logging.INFO)


def _build_config():
    listeners = {
        "default": {
            "type": "tcp",
            "bind": "0.0.0.0:1883",
        },
    }

    cert_dir = os.path.join(os.path.dirname(__file__), "certs")
    certfile = os.path.join(cert_dir, "server.crt")
    keyfile = os.path.join(cert_dir, "server.key")
    if os.path.exists(certfile) and os.path.exists(keyfile):
        listeners["tls"] = {
            "type": "tcp",
            "bind": "0.0.0.0:8883",
            "ssl": True,
            "certfile": certfile,
            "keyfile": keyfile,
        }

    return {
        "listeners": listeners,
        "sys_interval": 10,
        "auth": {
            "allow-anonymous": True,
        },
    }


def main():
    config = _build_config()

    async def run_broker():
        broker = Broker(config)
        await broker.start()
        binds = [l["bind"] for l in config["listeners"].values()]
        print(f"MQTT broker listening on {', '.join(binds)} (Ctrl+C to stop)")
        try:
            await asyncio.Event().wait()
        except KeyboardInterrupt:
            pass
        finally:
            await broker.shutdown()

    asyncio.run(run_broker())


if __name__ == "__main__":
    main()

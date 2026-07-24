#!/usr/bin/env python3
r"""Local MQTT broker for ESP32 AT Node testing.

  Uses amqtt to run a lightweight broker on localhost:1883.
  No authentication, no TLS — suitable for local development only.

  Usage:
      uv run python tools/mqtt_broker.py
"""
import asyncio
import logging

from amqtt.broker import Broker

logging.basicConfig(level=logging.INFO)


CONFIG = {
    "listeners": {
        "default": {
            "type": "tcp",
            "bind": "0.0.0.0:1883",
        },
    },
    "sys_interval": 10,
    "auth": {
        "allow-anonymous": True,
    },
}


def main():
    async def run_broker():
        broker = Broker(CONFIG)
        await broker.start()
        print("MQTT broker listening on 127.0.0.1:1883 (Ctrl+C to stop)")
        try:
            await asyncio.Event().wait()
        except KeyboardInterrupt:
            pass
        finally:
            await broker.shutdown()

    asyncio.run(run_broker())


if __name__ == "__main__":
    main()

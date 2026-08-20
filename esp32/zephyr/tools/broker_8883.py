#!/usr/bin/env python3
"""TLS MQTT broker for the nanoESP32-S3 demo, port 8883 (plain TLS, anonymous).

Based on at-node tools/broker/mqtt_broker.py (amqtt). Binds 0.0.0.0:8883 so the
ESP32 can reach it directly on the bridged LAN (VM IP 192.168.1.42).
"""
import asyncio
import logging
import os

from amqtt.broker import Broker

logging.basicConfig(level=logging.DEBUG)

CERT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "certs")

config = {
    "listeners": {
        "default": {
            "type": "tcp",
            "bind": "0.0.0.0:8883",
            "ssl": True,
            "certfile": f"{CERT_DIR}/server.crt",
            "keyfile": f"{CERT_DIR}/server.key",
        },
    },
    "sys_interval": 10,
    "auth": {"allow-anonymous": True},
}


async def run_broker():
    broker = Broker(config)
    await broker.start()
    print("MQTT-TLS broker listening on 0.0.0.0:8883")
    try:
        await asyncio.Event().wait()
    except KeyboardInterrupt:
        pass
    finally:
        await broker.shutdown()


if __name__ == "__main__":
    asyncio.run(run_broker())

#!/usr/bin/env python3
"""MQTT-TLS test client: subscribes to atnode/+/state,info,resp and
optionally publishes one AT command to atnode/<name>/cmd."""
import asyncio
import ssl
import sys

from amqtt.client import MQTTClient

BROKER = "ssl://192.168.1.42:8883"
CA = "esp32/zephyr/certs/ca.crt"


async def main():
    cmd_topic, cmd_payload = None, None
    if len(sys.argv) == 3:
        cmd_topic, cmd_payload = sys.argv[1], sys.argv[2]

    c = MQTTClient(config={"check_hostname": False})
    await c.connect(BROKER, cafile=CA)
    await c.subscribe([
        ("atnode/+/state", 0), ("atnode/+/info", 0),
        ("atnode/+/resp", 0), ("atnode/+/cmd", 0),
    ])
    if cmd_topic:
        await c.publish(cmd_topic, cmd_payload.encode())
        print(f">>> published {cmd_topic} <- {cmd_payload}", flush=True)

    end = asyncio.get_running_loop().time() + (15 if cmd_topic else 8)
    while asyncio.get_running_loop().time() < end:
        try:
            m = await c.deliver_message(timeout=1)
        except asyncio.TimeoutError:
            continue
        p = m.publish_packet
        print(f"RX [{p.topic_name}] {p.payload.data.decode(errors='replace')}", flush=True)
    await c.disconnect()


asyncio.run(main())

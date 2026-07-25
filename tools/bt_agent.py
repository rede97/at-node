#!/usr/bin/env python3
"""bt_agent.py — BlueZ pairing agent (NoInputNoOutput / Just Works).

Registers a D-Bus agent on the system bus and auto-accepts everything,
which is exactly what a BLE HID keyboard with Just Works pairing needs.
bluetoothctl's built-in agent registration fails on this VM
("Failed to register agent object"), so we do it ourselves (2026-07-24).

Usage:
    uv run python tools/bt_agent.py &        # leave running
    bluetoothctl pair <MAC>                  # pair in another shell
    # or one-shot:
    uv run python tools/bt_agent.py --pair <MAC>
"""
import asyncio
import sys

from dbus_next.aio import MessageBus
from dbus_next.service import ServiceInterface, method
from dbus_next import BusType

AGENT_PATH = "/atnode/agent"
CAPABILITY = "NoInputNoOutput"


class Agent(ServiceInterface):
    def __init__(self):
        super().__init__("org.bluez.Agent1")

    @method()
    def Release(self):
        print("[agent] released by bluez", flush=True)

    @method()
    def RequestPinCode(self, device: "o") -> "s":  # noqa: F821
        print(f"[agent] RequestPinCode {device} -> 0000", flush=True)
        return "0000"

    @method()
    def RequestPasskey(self, device: "o") -> "u":  # noqa: F821
        print(f"[agent] RequestPasskey {device} -> 0", flush=True)
        return 0

    @method()
    def DisplayPasskey(self, device: "o", passkey: "u", entered: "q"):  # noqa: F821
        print(f"[agent] DisplayPasskey {device} {passkey:06d}", flush=True)

    @method()
    def DisplayPinCode(self, device: "o", pincode: "s"):  # noqa: F821
        print(f"[agent] DisplayPinCode {device} {pincode}", flush=True)

    @method()
    def RequestConfirmation(self, device: "o", passkey: "u"):  # noqa: F821
        print(f"[agent] RequestConfirmation {device} {passkey:06d} -> OK", flush=True)

    @method()
    def RequestAuthorization(self, device: "o"):  # noqa: F821
        print(f"[agent] RequestAuthorization {device} -> OK", flush=True)

    @method()
    def AuthorizeService(self, device: "o", uuid: "s"):  # noqa: F821
        print(f"[agent] AuthorizeService {device} {uuid} -> OK", flush=True)

    @method()
    def Cancel(self):
        print("[agent] request cancelled", flush=True)


async def main():
    pair_mac = None
    if len(sys.argv) >= 3 and sys.argv[1] == "--pair":
        pair_mac = sys.argv[2].upper()

    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    bus.export(AGENT_PATH, Agent())

    def call(iface, method, *args):
        return bus.call(
            __import__("dbus_next").Message(
                destination="org.bluez",
                path="/org/bluez",
                interface=f"org.bluez.{iface}",
                member=method,
                signature={"RegisterAgent": "os", "RequestDefaultAgent": "o"}.get(method, ""),
                body=list(args),
            )
        )

    r = await call("AgentManager1", "RegisterAgent", AGENT_PATH, CAPABILITY)
    if r.message_type.name == "ERROR":
        print(f"[agent] RegisterAgent FAILED: {r.body}", flush=True)
        return 1
    await call("AgentManager1", "RequestDefaultAgent", AGENT_PATH)
    print(f"[agent] registered as default ({CAPABILITY})", flush=True)

    if pair_mac:
        dev_path = "/org/bluez/hci1/dev_" + pair_mac.replace(":", "_")
        from dbus_next import Message
        r = await bus.call(Message(
            destination="org.bluez", path=dev_path,
            interface="org.bluez.Device1", member="Pair"))
        if r.message_type.name == "ERROR":
            print(f"[agent] Pair FAILED: {r.body}", flush=True)
            return 1
        print(f"[agent] paired {pair_mac}", flush=True)
        return 0

    print("[agent] waiting for pairing requests... (Ctrl-C to quit)", flush=True)
    await asyncio.get_event_loop().create_future()  # run forever


if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)

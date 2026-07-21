#!/usr/bin/env python3
r"""Dongle phase-2 hardening tests (PLAN.md M2).

  Boards are auto-detected by the AT+VER role tag (same as
  test_dongle_loop.py — run that first; a bond must exist, though
  this script can also establish the link itself).

  Phases:
    0. ensure armed  — wait out AUTOCONN or scan+connect if idle
    1. AT+BT_LIST    — bond store readable, entry format valid
    2. auto-reconnect — kbd drops the link with AT+BT_DISC (immediate
                        LL terminate); dongle must re-attach on its own
                        (+BT_AUTO -> armed) and forward keys again.
                        NOTE: kbd AT+RST is NOT used as the trigger —
                        it only tests the supervision-timeout path and
                        proved unreliable on old kbd firmware.
    3. hold semantics — dongle AT+BT_DISC suppresses auto-reconnect
                        (one shot); AT+BT_AUTO=1 re-arms and the link
                        comes back
  Exit 0 = all phases pass.
"""
import re
import sys
import time

from test_dongle_loop import open_role, cmd, listen

RE_LIST = re.compile(r"\+BT_LIST:(\d+),([0-9A-F]{12}),flags=([0-9A-F]{4})")


def wait_armed(dgl, timeout=20):
    """Wait until the dongle reaches armed (state 4)."""
    end = time.time() + timeout
    buf = ""
    while time.time() < end:
        buf += listen(dgl, 0.5)
        if "armed" in buf:
            return True
        resp = cmd(dgl, b"AT+BT_STATE", 0.4)
        if "state=4" in resp:
            return True
    return False


def ensure_armed(dgl, kbd):
    """Get the dongle into armed state however we can."""
    resp = cmd(dgl, b"AT+BT_STATE", 0.5)
    if "state=4" in resp:
        return True
    if "state=5" in resp:          # AUTOCONN in flight — let it finish
        return wait_armed(dgl, 20)
    # idle — manual scan + connect (hold from a prior BT_DISC is fine,
    # a successful connection clears it)
    cmd(dgl, b"AT+BT_SCAN=5", 0.2)
    text = listen(dgl, 8)
    idx = None
    for line in text.splitlines():
        m = re.search(r"\+BT_SCAN:(\d+),.*AT-Node", line)
        if m:
            idx = m.group(1)
    if idx is None:
        print("  AT-Node keyboard not found in scan")
        return False
    cmd(dgl, f"AT+BT_CONN={idx}".encode(), 0.2)
    text = listen(dgl, 15)
    return "armed" in text


def key_check(dgl, kbd, kc=4, name="a"):
    """Inject a key on kbd, expect the report bytes on dongle."""
    dgl.reset_input_buffer()
    cmd(kbd, f"AT+KEY=0,{kc}".encode(), 0.3)
    cmd(kbd, b"AT+KEY=0", 0.3)
    seen = listen(dgl, 3)
    return f" {kc:02X} " in seen, seen


def main():
    results = []

    kbd = open_role("kbd")
    dgl = open_role("dongle")
    if not kbd or not dgl:
        print("FAIL: need both boards ([kbd] and [dongle] AT+VER tags)")
        return 1

    # kbd: BLE only, so injected keys don't also type on this PC over USB
    cmd(kbd, b"AT+KB=BLE", 0.5)

    # ---- phase 0: ensure armed --------------------------------------
    print("== phase 0: ensure armed ==")
    if not ensure_armed(dgl, kbd):
        print("  [FAIL] could not reach armed state")
        return 1
    print("  [ok] dongle armed")

    # ---- phase 1: AT+BT_LIST -----------------------------------------
    print("== phase 1: AT+BT_LIST ==")
    resp = cmd(dgl, b"AT+BT_LIST", 1.0)
    print(" ", resp.strip().replace("\r\n", " | "))
    m = RE_LIST.search(resp)
    ok = m is not None
    print(f"  [{'PASS' if ok else 'FAIL'}] bond entry with valid format")
    results.append(("BT_LIST", ok))

    # ---- phase 2: auto-reconnect after remote disconnect --------------
    print("== phase 2: auto-reconnect (kbd AT+BT_DISC) ==")
    dgl.reset_input_buffer()
    cmd(kbd, b"AT+BT_DISC", 0.5)
    out = listen(dgl, 25)
    for line in out.splitlines():
        if line.startswith("+BT"):
            print("  ", line)
    ok_term = "+BT_DISC: reason" in out
    ok_auto = "reconnecting" in out or "+BT_AUTO: connected" in out
    ok_arm = "armed" in out
    print(f"  [{'PASS' if ok_term else 'FAIL'}] terminate seen (+BT_DISC: reason)")
    print(f"  [{'PASS' if ok_auto else 'FAIL'}] auto-reconnect kicked (+BT_AUTO)")
    print(f"  [{'PASS' if ok_arm else 'FAIL'}] re-armed")
    results.append(("auto-reconnect", ok_term and ok_auto and ok_arm))
    if ok_arm:
        ok_key, seen = key_check(dgl, kbd)
        print(f"  [{'PASS' if ok_key else 'FAIL'}] key forwarded after reconnect")
        results.append(("post-reconnect key", ok_key))

    # ---- phase 3: hold semantics --------------------------------------
    print("== phase 3: hold (dongle AT+BT_DISC) ==")
    cmd(dgl, b"AT+BT_DISC", 0.5)
    out = listen(dgl, 8)
    for line in out.splitlines():
        if line.startswith("+BT"):
            print("  ", line)
    ok_hold = "reconnecting" not in out
    print(f"  [{'PASS' if ok_hold else 'FAIL'}] no auto-reconnect while held")
    results.append(("hold", ok_hold))

    cmd(dgl, b"AT+BT_AUTO=1", 0.5)
    out = listen(dgl, 25)
    for line in out.splitlines():
        if line.startswith("+BT"):
            print("  ", line)
    ok_rearm = "armed" in out
    print(f"  [{'PASS' if ok_rearm else 'FAIL'}] AT+BT_AUTO=1 re-arms, link back")
    results.append(("re-arm", ok_rearm))
    if ok_rearm:
        ok_key, seen = key_check(dgl, kbd)
        print(f"  [{'PASS' if ok_key else 'FAIL'}] key forwarded after re-arm")
        results.append(("post-rearm key", ok_key))

    # ---- summary -------------------------------------------------------
    print("\n== summary ==")
    all_ok = True
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
        all_ok &= ok
    print("\nALL PASS" if all_ok else "\nSOME FAILED")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())

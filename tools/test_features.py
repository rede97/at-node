#!/usr/bin/env python3
r"""Feature regression — post-M5 AT features + link supervision (C1/C2).

  Covers (two-board rig, auto-detected by AT+VER role tags):
    1. AT+STATUS          -> prints role/kb/ble/batt line
    2. AT+HELP=<CMD>      -> single-line help for a known command
    3. URC (F3.6)         -> kbd emits +BT_CONNECTED / +BT_DISCONNECTED
    4. AT+KEY_STR         -> string reaches dongle as key reports (e2e)
    5. C1 supervision     -> kbd hard-vanishes (AT+RST), dongle must
                             report +BT_DISC reason (LL timeout) < 20 s
    6. C2 supervision     -> dongle hard-vanishes, kbd must drop within
                             <timeout>s. KNOWN OPEN ISSUE (2026-07-22):
                             peripheral-side LL supervision never fires
                             (seen hanging 60 s+). Reported as XFAIL.

  Exit 0 = all pass (C2 excluded from gating until fixed).
"""
import re
import sys
import time

from test_dongle_loop import open_role, cmd, listen


def main():
    results = []
    kbd = open_role("kbd")
    dgl = open_role("dongle")
    if not kbd or not dgl:
        print("FAIL: need both boards ([kbd]/[dongle])")
        return 1

    # ---- 1. AT+STATUS --------------------------------------------------
    print("== AT+STATUS ==")
    resp = cmd(kbd, b"AT+STATUS", 1.0)
    print(" ", resp.strip().replace("\r\n", " | "))
    ok = "role=kbd" in resp and "batt=" in resp
    results.append(("STATUS", ok))
    print(f"  [{'PASS' if ok else 'FAIL'}]")

    # ---- 2. AT+HELP=<CMD> -----------------------------------------------
    print("== AT+HELP=<CMD> ==")
    resp = cmd(kbd, b"AT+HELP=AT+TAP", 1.0)
    ok = "press+release" in resp and "Commands:" not in resp
    results.append(("HELP=<CMD>", ok))
    print(f"  [{'PASS' if ok else 'FAIL'}] {resp.strip()!r}")
    resp = cmd(kbd, b"AT+HELP=AT+NOPE", 1.0)
    ok2 = "unknown command" in resp
    results.append(("HELP= unknown", ok2))
    print(f"  [{'PASS' if ok2 else 'FAIL'}] unknown-cmd rejection")

    # ---- link up for URC / KEY_STR / C1 --------------------------------
    resp = cmd(dgl, b"AT+BT_STATE", 0.5)
    if "state=4" not in resp:
        cmd(dgl, b"AT+BT_AUTO=1", 0.5)
        end = time.time() + 15
        while time.time() < end and "state=4" not in cmd(dgl, b"AT+BT_STATE", 0.4):
            time.sleep(0.5)

    # ---- 3. URC ----------------------------------------------------------
    print("== URC (F3.6) ==")
    resp = cmd(kbd, b"AT+KB", 0.6)
    if "connected" not in resp:
        results.append(("URC (link up)", False))
        print("  [SKIP] kbd not connected")
    else:
        kbd.reset_input_buffer()
        kbd.write(b"AT+BT_DISC\r\n")   # raw write: cmd() would eat the fast URC
        out = listen(kbd, 15)
        ok_disc = "+BT_DISCONNECTED" in out
        ok_conn = "+BT_CONNECTED" in out
        results.append(("URC disconnect", ok_disc))
        results.append(("URC reconnect", ok_conn))
        print(f"  [{'PASS' if ok_disc else 'FAIL'}] +BT_DISCONNECTED")
        print(f"  [{'PASS' if ok_conn else 'FAIL'}] +BT_CONNECTED (auto-reconnect)")

    # ---- 4. KEY_STR e2e ---------------------------------------------------
    print("== AT+KEY_STR e2e ==")
    end = time.time() + 15
    while time.time() < end and "state=4" not in cmd(dgl, b"AT+BT_STATE", 0.4):
        time.sleep(0.5)
    cmd(kbd, b"AT+KB=BLE", 0.4)
    dgl.reset_input_buffer()
    kbd.write(b"AT+KEY_STR=aS1!\r\n")
    seen = listen(dgl, 6)
    presses = [l for l in seen.splitlines()
               if l.startswith("+BT_NTF") and not l.endswith("00 00 00 00 00 00 00")]
    ok = len(presses) >= 4
    results.append(("KEY_STR e2e", ok))
    print(f"  [{'PASS' if ok else 'FAIL'}] {len(presses)} press reports (expect >=4)")

    # ---- 5. C1 central-side supervision -----------------------------------
    print("== C1: kbd vanishes -> dongle LL timeout ==")
    end = time.time() + 15
    while time.time() < end and "state=4" not in cmd(dgl, b"AT+BT_STATE", 0.4):
        time.sleep(0.5)
    dgl.reset_input_buffer()
    kbd.write(b"AT+RST\r\n")
    seen = listen(dgl, 20)
    ok = "+BT_DISC: reason" in seen
    results.append(("C1 central supervision", ok))
    print(f"  [{'PASS' if ok else 'FAIL'}] {'got terminate' if ok else 'silent 20s'}")

    # ---- 6. C2 peripheral-side supervision (KNOWN OPEN) -------------------
    print("== C2: dongle vanishes -> kbd drops (known-open) ==")
    time.sleep(3)
    kbd2 = open_role("kbd")          # kbd rebooted in C1 — reopen
    if kbd2:
        cmd(kbd2, b"AT+KB", 0.5)
        dgl2 = open_role("dongle")
        if dgl2:
            cmd(dgl2, b"AT+BT_AUTO=1", 0.5)
            time.sleep(6)            # let the link come back
        before = cmd(kbd2, b"AT+KB", 0.6)
        if "connected" in before and dgl2:
            dgl2.write(b"AT+RST\r\n")
            time.sleep(20)
            after = cmd(kbd2, b"AT+KB", 0.8)
            ok = "disconnected" in after
        else:
            ok = None
        if ok is None:
            print("  [SKIP] link not re-established")
        else:
            print(f"  [{'PASS' if ok else 'XFAIL-known-open'}] "
                  f"{'dropped <20s' if ok else 'still connected 20s+ (peripheral LL supervision issue)'}")
            results.append(("C2 peripheral supervision", ok))
        kbd2.close()

    print("\n== summary ==")
    gate = True
    for name, ok in results:
        mark = "PASS" if ok else ("XFAIL" if name.startswith("C2") else "FAIL")
        print(f"  [{mark}] {name}")
        if not name.startswith("C2"):
            gate &= bool(ok)
    print("\nALL PASS" if gate else "\nSOME FAILED")
    return 0 if gate else 1


if __name__ == "__main__":
    sys.exit(main())

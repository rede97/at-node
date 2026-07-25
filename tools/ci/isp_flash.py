#!/usr/bin/env python3
r"""ISP firmware downloader — two threads, catches the 10 s boot ROM window.

  usage: uv run python tools/ci/isp_flash.py <hex> [--port /dev/ttyACMx]
                                             [--attempts N] [--erase-eeprom]

  Field lesson (2026-07-21): the CH582 boot ROM ISP window is ~10 s and
  the USB handshake is racy — do NOT probe-then-act step by step (the
  window expires while you think). Two threads run in parallel:

    flasher  hammers `wchisp flash` in a loop bounded by --attempts.
             Each attempt fails fast (~50 ms) when no ISP device is
             present; the moment the board enumerates, the CURRENT
             attempt lands the handshake.
    trigger  --port sends AT+ISP to a board RUNNING our firmware
             (erases page0 + resets into ISP). Without --port, just
             plug in / reset a blank or BOOT-buttoned board yourself.

  --erase-eeprom  wipe DataFlash (SNV bonds, slotmap, role) before flash.
                  Two ISP rounds: first erases eeprom (chip re-enters ISP
                  as blank), second flashes the firmware.

  CH582 ISP enumerates as 4348:55e0 (NOT 1a86:8010 — that's WCH-Link).
  Recovery if all attempts miss: chip is blank — reset/re-plug opens a
  fresh window; wlink works regardless.

  Exit 0 = flashed + verified, 1 = attempts exhausted.
"""
import argparse
import subprocess
import sys
import threading
import time


def hammer_wchisp(args_list, success_marker, attempts, label, result):
    """Run wchisp in a retry loop until success or exhausted."""
    for n in range(1, attempts + 1):
        if result["done"]:
            return
        p = subprocess.run(["wchisp"] + args_list,
                           capture_output=True, text=True)
        out = p.stdout + p.stderr
        if p.returncode == 0 and success_marker in out:
            result.update(done=True, ok=True)
            print(f"[{label}] SUCCESS on attempt {n}")
            print(out)
            return
        if "No WCH ISP" not in out and "Entity not found" not in out:
            print(f"[{label}] attempt {n}: {out.strip()[-80:]}")
        time.sleep(0.5)
    print(f"[{label}] exhausted {attempts} attempts")


def flasher(hex_path, attempts, result):
    hammer_wchisp(["flash", hex_path], "Verify OK", attempts, "flasher", result)


def eeprom_eraser(attempts, result):
    hammer_wchisp(["eeprom", "erase"], "EEPROM erased", attempts, "eeprom", result)


def send_at_isp(port):
    import serial
    print(f"[trigger] sending AT+ISP to {port}")
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        s.write(b"\r\n")
        time.sleep(0.3)
        s.reset_input_buffer()
        s.write(b"AT+ISP\r\n")
        time.sleep(0.5)
        try:
            print("[trigger] board says:",
                  s.read(s.in_waiting or 1).decode(errors="replace").strip())
        except OSError:
            print("[trigger] port vanished (chip reset into ISP)")
        s.close()
    except serial.SerialException as e:
        print(f"[trigger] WARNING: {e} — flash a firmware with AT+ISP "
              f"support first, or enter ISP manually", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hex", help="firmware image, e.g. tools/ci/out/kbd.hex")
    ap.add_argument("--port", help="serial port of a RUNNING board to "
                                   "trigger via AT+ISP (optional)")
    ap.add_argument("--erase-eeprom", action="store_true",
                    help="wipe DataFlash (SNV bonds, slotmap, role) "
                         "before flashing")
    ap.add_argument("--attempts", type=int, default=40)
    args = ap.parse_args()

    result = {"done": False, "ok": False}

    if args.erase_eeprom:
        # Round 1: erase DataFlash (board re-enters ISP as blank chip)
        print(f"[isp_flash] erasing DataFlash (max {args.attempts} attempts)...")
        t = threading.Thread(target=eeprom_eraser, args=(args.attempts, result))
        t.start()
        if args.port:
            send_at_isp(args.port)
        else:
            print("[trigger] no --port — enter ISP manually")
        t.join()
        if not result["ok"]:
            print("[isp_flash] eeprom erase failed")
            return 1
        # Board reset from erase; blank chip auto-enters ISP.
        # Give it a moment to re-enumerate.
        time.sleep(1.5)

    # Round 2 (or 1): flash firmware
    result = {"done": False, "ok": False}
    print(f"[isp_flash] hammering wchisp flash (max {args.attempts} attempts)...")
    t = threading.Thread(target=flasher, args=(args.hex, args.attempts, result))
    t.start()

    if args.port and not args.erase_eeprom:
        send_at_isp(args.port)
    elif args.erase_eeprom:
        # After eeprom erase, blank chip auto-enters ISP on reset —
        # no AT+ISP needed (firmware is blank anyway)
        print("[trigger] blank chip auto-enters ISP, no AT+ISP needed")
    else:
        print("[trigger] no --port — enter ISP manually now "
              "(BOOT button / blank chip)")

    t.join()
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

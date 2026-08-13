import subprocess, time, re, sys

target = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.27"
dur = int(sys.argv[2]) if len(sys.argv) > 2 else 300
t0 = time.time()
while time.time() - t0 < dur:
    ts = time.strftime("%H:%M:%S")
    try:
        out = subprocess.run(["ping", "-n", "1", "-w", "2000", target],
                             capture_output=True, text=True, timeout=5).stdout
        m = re.search(r"[=<](\d+)ms", out)
        ms = m.group(1) if m else "TIMEOUT"
    except Exception:
        ms = "ERR"
    print(f"{ts} {ms}ms", flush=True)
    time.sleep(1)

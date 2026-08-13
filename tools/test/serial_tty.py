import serial, sys, threading, time

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
ser = serial.Serial(port, 115200, timeout=0.1)
stop = False

def reader():
    while not stop:
        try:
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        except Exception as e:
            print(f"[reader err: {e}]")
            break

t = threading.Thread(target=reader, daemon=True)
t.start()

for line in sys.stdin:
    line = line.rstrip("\n")
    if line == "EXIT":
        break
    ser.write(line.encode() + b"\r")
    ser.flush()

stop = True
ser.close()

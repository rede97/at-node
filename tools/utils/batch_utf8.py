#!/usr/bin/env python3
"""Convert WCH SDK files from GB2312 to UTF-8."""
import os, sys, glob

ROOT = sys.argv[1] if len(sys.argv) > 1 else "software"

converted = 0
for ext in ("*.c", "*.h", "*.S"):
    for path in sorted(glob.glob(os.path.join(ROOT, "**", ext), recursive=True)):
        with open(path, "rb") as f:
            raw = f.read()
        try:
            raw.decode("utf-8")
            continue  # already UTF-8
        except UnicodeDecodeError:
            pass
        # Try GB2312 / GBK / GB18030
        for enc in ("gb2312", "gbk", "gb18030"):
            try:
                text = raw.decode(enc)
                raw = text.encode("utf-8")
                with open(path, "wb") as f:
                    f.write(raw)
                converted += 1
                print(f"{enc}: {path}")
                break
            except UnicodeDecodeError:
                continue
        else:
            print(f"SKIP: {path} — unknown encoding")
            sys.exit(1)

print(f"\nDone — {converted} files converted to UTF-8")

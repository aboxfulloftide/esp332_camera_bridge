#!/usr/bin/env python3
"""
Receive a JPEG photo from HT-HC32 over serial and save it to disk.
Usage: python3 receive_photo.py [port] [output.jpg]
"""
import serial
import sys
import time
import os

PORT    = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
OUTFILE = sys.argv[2] if len(sys.argv) > 2 else "photo.jpg"
BAUD    = 921600

def main():
    print(f"Opening {PORT} at {BAUD} baud...")
    with serial.Serial(PORT, BAUD, timeout=10,
                       dsrdtr=False, rtscts=False, xonxoff=False) as ser:
        time.sleep(0.5)
        print("Sending capture command...")
        ser.write(b'c')
        ser.flush()

        print("Waiting for JPEG_START...")
        while True:
            line = ser.readline().decode(errors="replace").strip()
            if line.startswith("JPEG_START:"):
                length = int(line.split(":")[1])
                print(f"  Receiving {length} bytes...")
                break
            if line:
                print(f"  {line}")

        data = b""
        while len(data) < length:
            chunk = ser.read(length - len(data))
            if not chunk:
                print("Timeout reading image data!")
                return
            data += chunk

        end_line = ser.readline().decode(errors="replace").strip()
        if end_line != "JPEG_END":
            print(f"Warning: expected JPEG_END, got: {end_line!r}")

        with open(OUTFILE, "wb") as f:
            f.write(data)
        print(f"Saved {len(data)} bytes to {OUTFILE}")

if __name__ == "__main__":
    main()

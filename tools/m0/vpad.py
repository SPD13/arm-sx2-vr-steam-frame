#!/usr/bin/env python3
"""Virtual Xbox 360-style gamepad through Linux uinput (needs write access to /dev/uinput).
Taps buttons at given times so an emulator can be driven without a person holding a controller.

Usage: vpad.py --hold <seconds> <seconds>:<button>[:<held seconds>] ...
  buttons: a b x y lb rb back start guide up down left right
The device exists from start until --hold seconds have passed.
"""
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_SETUP, UI_ABS_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x401C5504, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BUTTONS = {"a": 0x130, "b": 0x131, "x": 0x133, "y": 0x134, "lb": 0x136, "rb": 0x137,
           "back": 0x13A, "start": 0x13B, "guide": 0x13C, "ls": 0x13D, "rs": 0x13E}
HAT = {"up": (0x11, -1), "down": (0x11, 1), "left": (0x10, -1), "right": (0x10, 1)}
STICKS = [(0x00, -32768, 32767), (0x01, -32768, 32767), (0x03, -32768, 32767), (0x04, -32768, 32767),
          (0x02, 0, 255), (0x05, 0, 255), (0x10, -1, 1), (0x11, -1, 1)]


def emit(fd, etype, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, etype, code, value))


def main():
    args = sys.argv[1:]
    hold = 0.0
    if args[:1] == ["--hold"]:
        hold, args = float(args[1]), args[2:]
    events = []
    for a in args:
        parts = a.split(":")
        events.append((float(parts[0]), parts[1].lower(), float(parts[2]) if len(parts) > 2 else 0.15))
    events.sort()

    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    for code in BUTTONS.values():
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    for code, lo, hi in STICKS:
        fcntl.ioctl(fd, UI_SET_ABSBIT, code)
        fcntl.ioctl(fd, UI_ABS_SETUP, struct.pack("HH6i", code, 0, 0, lo, hi, 0, 0, 0))
    # BUS_USB, Microsoft Xbox 360 controller ids so SDL's controller DB maps it.
    fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("4H80sI", 0x03, 0x045E, 0x028E, 0x0114, b"Xbox 360 Controller (vpad)", 0))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    start = time.monotonic()
    try:
        for at, button, held in events:
            time.sleep(max(0.0, start + at - time.monotonic()))
            if button in HAT:
                code, value = HAT[button]
                press, release = (EV_ABS, code, value), (EV_ABS, code, 0)
            elif button in BUTTONS:
                press, release = (EV_KEY, BUTTONS[button], 1), (EV_KEY, BUTTONS[button], 0)
            else:
                print(f"unknown button {button}", file=sys.stderr)
                continue
            emit(fd, *press); emit(fd, EV_SYN, 0, 0)
            time.sleep(held)
            emit(fd, *release); emit(fd, EV_SYN, 0, 0)
            print(f"{time.monotonic() - start:6.1f}s {button}", flush=True)
        time.sleep(max(0.0, start + hold - time.monotonic()))
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


if __name__ == "__main__":
    main()

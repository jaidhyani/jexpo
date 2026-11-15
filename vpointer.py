#!/usr/bin/python3
"""vpointer: a virtual mouse for a Wayland compositor (zwlr_virtual_pointer_v1), for driving
jexpo in a nested Hyprland the way a hand would. Connects to $WAYLAND_DISPLAY.

    vpointer.py W H move X Y | press | release | click X Y | drag X1 Y1 X2 Y2 [steps] ...

W H are the output's size (absolute motion is a fraction of it). Commands run in order; "sleep S"
pauses. Speaks the Wayland wire protocol itself, no bindings needed.
"""
import os
import socket
import struct
import sys
import time


def _string(s):
    data = s.encode() + b"\0"
    return struct.pack("=I", len(data)) + data + b"\0" * (-len(data) % 4)


class _Connection:
    """The least of a Wayland client: send requests, read events, hand each to its object."""

    def __init__(self):
        display = os.environ.get("WAYLAND_DISPLAY", "wayland-0")
        path = display if display.startswith("/") else os.path.join(os.environ["XDG_RUNTIME_DIR"], display)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.last_id = 1  # wl_display
        self.listeners = {}
        self.pending = b""

    def new_id(self, listener=None):
        self.last_id += 1
        if listener:
            self.listeners[self.last_id] = listener
        return self.last_id

    def send(self, obj, opcode, payload=b""):
        self.sock.sendall(struct.pack("=II", obj, (8 + len(payload)) << 16 | opcode) + payload)

    def roundtrip(self):
        done = []
        self.send(1, 0, struct.pack("=I", self.new_id(lambda op, body: done.append(True))))  # wl_display.sync
        while not done:
            data = self.sock.recv(1 << 16)
            if not data:
                sys.exit("vpointer: compositor closed the connection")
            self.pending += data
            while len(self.pending) >= 8:
                obj, word = struct.unpack_from("=II", self.pending)
                size, opcode = word >> 16, word & 0xFFFF
                if len(self.pending) < size:
                    break
                body, self.pending = self.pending[8:size], self.pending[size:]
                if obj in self.listeners:
                    self.listeners[obj](opcode, body)


MANAGER = "zwlr_virtual_pointer_manager_v1"
CREATE = 0                     # manager.create_virtual_pointer(seat, id)
MOTION_ABS, BUTTON, FRAME = 1, 2, 4
BTN_LEFT, BTN_RIGHT = 0x110, 0x111


def main(argv):
    width, height = int(argv[0]), int(argv[1])
    conn = _Connection()
    globals_ = {}

    def on_global(opcode, body):
        if opcode == 0:
            name, length = struct.unpack_from("=II", body)
            globals_[body[8:8 + length - 1].decode()] = name

    registry = conn.new_id(on_global)
    conn.send(1, 1, struct.pack("=I", registry))
    conn.roundtrip()
    manager = conn.new_id()
    conn.send(registry, 0, struct.pack("=I", globals_[MANAGER]) + _string(MANAGER) + struct.pack("=II", 1, manager))
    pointer = conn.new_id()
    conn.send(manager, CREATE, struct.pack("=II", 0, pointer))
    conn.roundtrip()

    def ms():
        return int(time.monotonic() * 1000) & 0xFFFFFFFF

    def move(x, y):
        conn.send(pointer, MOTION_ABS, struct.pack("=IIIII", ms(), int(x), int(y), width, height))
        conn.send(pointer, FRAME)
        conn.roundtrip()

    def button(b, down):
        conn.send(pointer, BUTTON, struct.pack("=III", ms(), b, 1 if down else 0))
        conn.send(pointer, FRAME)
        conn.roundtrip()

    args = list(argv[2:])
    btn = BTN_LEFT
    while args:
        cmd = args.pop(0)
        if cmd == "right":
            btn = BTN_RIGHT
        elif cmd == "left":
            btn = BTN_LEFT
        elif cmd == "move":
            move(float(args.pop(0)), float(args.pop(0)))
        elif cmd == "press":
            button(btn, True)
        elif cmd == "release":
            button(btn, False)
        elif cmd == "sleep":
            time.sleep(float(args.pop(0)))
        elif cmd == "click":
            move(float(args.pop(0)), float(args.pop(0)))
            time.sleep(0.05)
            button(btn, True)
            time.sleep(0.05)
            button(btn, False)
        elif cmd == "drag":
            x1, y1, x2, y2 = (float(args.pop(0)) for _ in range(4))
            steps = int(args.pop(0)) if args and args[0].isdigit() else 30
            move(x1, y1)
            time.sleep(0.05)
            button(btn, True)
            for i in range(1, steps + 1):
                move(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps)
                time.sleep(0.016)
            time.sleep(0.1)
            button(btn, False)
        else:
            sys.exit(f"vpointer: unknown command {cmd}")
    conn.sock.close()


if __name__ == "__main__":
    main(sys.argv[1:])

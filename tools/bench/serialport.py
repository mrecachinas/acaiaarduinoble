"""Minimal POSIX serial access for the ShotStopper's USB serial port (stdlib only)."""
import fcntl
import os
import select
import struct
import termios
import time


def open_port(path, baud=termios.B9600, attempts=40):
    """Open the port raw, retrying briefly while it re-enumerates after a reset."""
    for attempt in range(attempts):
        try:
            fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            break
        except OSError:
            if attempt == attempts - 1:
                raise
            time.sleep(0.25)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = attrs[5] = baud
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def reset_board(fd):
    """Reset an ESP32 USB-Serial/JTAG board without entering the bootloader."""
    pack = lambda bits: struct.pack("I", bits)
    fcntl.ioctl(fd, termios.TIOCMBIC, pack(termios.TIOCM_DTR))
    fcntl.ioctl(fd, termios.TIOCMBIS, pack(termios.TIOCM_RTS))
    time.sleep(0.2)
    fcntl.ioctl(fd, termios.TIOCMBIC, pack(termios.TIOCM_RTS))


class LineReader:
    """Collects complete lines from a non-blocking port as (elapsed seconds, text)."""

    def __init__(self, fd):
        self.fd = fd
        self.buffer = ""
        self.start = time.monotonic()

    def restart_clock(self):
        self.start = time.monotonic()

    def elapsed(self):
        return time.monotonic() - self.start

    def poll(self, timeout=0.1):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return []
        try:
            self.buffer += os.read(self.fd, 4096).decode("utf-8", "replace")
        except (BlockingIOError, OSError):
            time.sleep(0.1)  # port briefly gone while the board resets
            return []
        lines = []
        while "\n" in self.buffer:
            line, self.buffer = self.buffer.split("\n", 1)
            line = line.strip()
            if line:
                lines.append((self.elapsed(), line))
        return lines


def is_weight(line):
    """Weight lines are numbers only, e.g. '12.30' or '12.30 4.10 26.50' (may include inf)."""
    for field in line.split():
        try:
            float(field)
        except ValueError:
            return False
    return bool(line.split())

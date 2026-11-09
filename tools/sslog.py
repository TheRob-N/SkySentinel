"""Log a SkySentinel board's serial output to a file, in the background.

    nohup python3 tools/sslog.py <port> ~/heltec.log > /dev/null 2>&1 &
    pkill -f "sslog.py /dev/serial"      # stop it (do this before flashing that board)

Keeps every line except status, which it keeps once a minute (enough to see memory
trends). Doesn't reset the board when it opens the port, and reconnects by itself
if the board restarts.

Sending the board a command: don't open its port from anything else (echo, a second
terminal, the web page) while the logger has it. On the C5's built-in USB that has
left the board's USB output stuck until a reset. Write to the logger's mailbox instead,
the log's name with .cmd, and the logger sends it down the port it already has open:

    echo "status" > ~/master.cmd
    echo "node A184 link normal" >> ~/master.cmd     (several lines are fine)

Sent commands appear in the log as "--- sent: ...".
"""
import datetime
import os
import sys
import time

import serial


def stamp():
    return f"{datetime.datetime.now():%Y-%m-%d %H:%M:%S}"


def single_instance(port):
    """Only one logger per board: two of them fight over the port, and on boards with
    their own USB (C5, Heltec) every fight resets the board."""
    import fcntl, hashlib, tempfile
    name = hashlib.sha1(os.path.realpath(port).encode()).hexdigest()[:12]
    lock = open(os.path.join(tempfile.gettempdir(), f"sslog-{name}.lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit(f"sslog: already logging {port} (stop it first: pkill -f \"sslog.py {port}\")")
    return lock


def mailbox_path(log_path):
    base, ext = os.path.splitext(log_path)
    return (base if ext == ".log" else log_path) + ".cmd"


def take_commands(box):
    """Lines waiting in the mailbox, emptied atomically (a writer can't lose a line)."""
    if not os.path.exists(box) or os.path.getsize(box) == 0:
        return []
    taking = box + ".sending"
    try:
        os.replace(box, taking)
        with open(taking, errors="replace") as f:
            lines = [l.strip() for l in f.read().splitlines()]
        os.remove(taking)
    except OSError:
        return []
    return [l for l in lines if l]


def main():
    port, path = sys.argv[1], sys.argv[2]
    box = mailbox_path(os.path.expanduser(path))
    _lock = single_instance(port)
    last_status = 0.0
    while True:
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, 115200, 1
            s.exclusive = True  # and nothing else (monitor, flasher) can grab it meanwhile
            # Don't reset the board. Linux raises DTR and RTS together when the port
            # opens, which is harmless (the auto-reset circuit cancels out). Setting
            # dtr/rts = False before open() made pyserial drop DTR first, leaving RTS
            # alone for a moment: exactly the "reset" combination. So open with both
            # up, then drop RTS first (DTR alone only touches the boot pin) and DTR last.
            s.open()
            try:
                s.rts = False
                s.dtr = False
            except (serial.SerialException, OSError):
                pass  # no modem lines (a pseudo-terminal): nothing to reset anyway
            with open(path, "a", buffering=1) as f:
                f.write(f"{stamp()} --- port open (commands: {box})\n")
                buf = b""
                while True:
                    for cmd in take_commands(box):
                        s.write((cmd + "\n").encode())
                        s.flush()
                        f.write(f"{stamp()} --- sent: {cmd}\n")
                        time.sleep(0.2)
                    buf += s.read(4096)
                    *lines, buf = buf.split(b"\n")
                    for raw in lines:
                        line = raw.decode("utf-8", "replace").rstrip()
                        if not line:
                            continue
                        if line.startswith('{"t":"status"'):
                            if time.time() - last_status < 60:
                                continue
                            last_status = time.time()
                        f.write(f"{stamp()} {line}\n")
        except (serial.SerialException, OSError) as e:
            with open(path, "a") as f:
                f.write(f"{stamp()} --- port lost ({e}), retrying\n")
            time.sleep(3)


if __name__ == "__main__":
    main()

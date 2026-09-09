"""Talk to the NRO's control socket (see ctl_init in main.c).

The card flags are ordinary files under sdmc:/switch/boz, but editing them
means ejecting the card, and a run that differs from its pair by a file nobody
noticed is an expensive mistake. This is the other end of the line protocol the
NRO already speaks: LS, GET, PUT and DEL, confined to that one directory.

Writes land immediately but take effect at the NEXT launch, because every flag
is read once during startup -- a flag that changed under a running measurement
would be worse than the problem it solves.

  python boz_ctl.py <ip> ls
  python boz_ctl.py <ip> put <name> [contents]
  python boz_ctl.py <ip> putfile <name> <local path>
  python boz_ctl.py <ip> get <name>
  python boz_ctl.py <ip> del <name>
"""
import io
import socket
import sys

PORT = 28772


def connect(ip):
    s = socket.create_connection((ip, PORT), timeout=8)
    s.settimeout(8)
    return s


def drain(s):
    out = b""
    try:
        while True:
            b = s.recv(65536)
            if not b:
                break
            out += b
    except socket.timeout:
        pass
    return out.decode("utf-8", "replace")


def main():
    ip, cmd = sys.argv[1], sys.argv[2].lower()
    s = connect(ip)
    if cmd == "ls":
        s.sendall(b"LS\n")
    elif cmd == "get":
        s.sendall(("GET %s\n" % sys.argv[3]).encode())
    elif cmd == "del":
        s.sendall(("DEL %s\n" % sys.argv[3]).encode())
    elif cmd == "putfile":
        # The NRO lives in this same directory, so a build can be deployed over
        # the control socket instead of ejecting the card. The device drains
        # the body in 8 KB chunks in a tight loop, so this goes at whatever the
        # network manages; it is the header that has to be exact, because the
        # device counts down from the length and any mismatch leaves it
        # treating the next command as file data.
        body = io.open(sys.argv[4], "rb").read()
        s.sendall(("PUT %s %d\n" % (sys.argv[3], len(body))).encode())
        s.sendall(body)
        print("sent %d bytes" % len(body))
    elif cmd == "put":
        body = (sys.argv[4] if len(sys.argv) > 4 else "").encode()
        s.sendall(("PUT %s %d\n" % (sys.argv[3], len(body))).encode())
        if body:
            s.sendall(body)
    else:
        raise SystemExit("unknown command " + cmd)
    print(drain(s), end="")
    s.close()


main()

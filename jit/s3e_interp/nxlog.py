"""Listen for the NRO's log without netloader.

nxlinkConnectToHost() just opens a TCP connection to __nxlink_host on
NXLINK_CLIENT_PORT and writes stdout/stderr to it -- no handshake, no framing.
So a plain socket server is a complete replacement for `nxlink -s`, and it does
not need the netloader, which is what has been failing (it writes the NRO to
the SD card, and that write errors with EIO).

Put this PC's IPv4 address in sdmc:/switch/boz/nxlink_host.txt, start this,
then launch the NRO from hbmenu.

One connection per thread, deliberately. The first version accepted a single
connection and only accepted the next one after that had closed -- which is
fine until the console goes away without closing the socket, as it does when
homebrew is force-quit or the app hangs and is killed. The server then sits in
recv() on a dead connection forever while the NEXT launch waits unaccepted in
the backlog, and the log simply stops. That looks exactly like a console that
crashed on startup, and it cost a debugging cycle to recognise. A thread per
connection cannot get into that state: a stale reader blocks only itself.
"""
import socket
import sys
import threading
import time

PORT = 28771            # NXLINK_CLIENT_PORT
log = sys.argv[1] if len(sys.argv) > 1 else "nxlog.txt"
lock = threading.Lock()


def reader(conn, addr):
    # The peer port distinguishes sessions if two ever overlap, which is the
    # situation this file exists to survive rather than to prevent.
    tag = "%s:%d" % (addr[0], addr[1])
    header = ("\n=== session %s from %s ===\n"
              % (time.strftime("%H:%M:%S"), tag)).encode()
    with lock:
        with open(log, "ab") as f:
            f.write(header)
            f.flush()
    sys.stdout.buffer.write(header)
    sys.stdout.buffer.flush()
    # Stamp each line with seconds since this session connected.
    #
    # The console sends an unframed byte stream with no timing in it, so every
    # question of the form "how long did that phase take" has had to be
    # answered by sampling the file from outside and subtracting -- which is
    # awkward, easy to get wrong, and impossible after the fact. A relative
    # stamp is better than a wall clock here: what is always wanted is elapsed
    # time since launch, and that subtraction should not have to be done by
    # hand. Partial lines are held back so a stamp only ever appears at a real
    # line start.
    t0 = time.time()
    # With two consoles connected at once their lines interleave in one file,
    # so each line also carries the last octet of the sender's address, after
    # the stamp: "[   12.34] @188 ...". Anything matching on the text after the
    # stamp keeps working.
    who = ("@%s " % addr[0].rsplit(".", 1)[-1]).encode()
    pending = b""
    try:
        while True:
            b = conn.recv(65536)
            if not b:
                break
            pending += b
            out = b""
            while True:
                nl = pending.find(b"\n")
                if nl < 0:
                    break
                line, pending = pending[:nl + 1], pending[nl + 1:]
                out += (b"[%8.2f] " % (time.time() - t0)) + who + line
            if not out:
                continue
            with lock:
                with open(log, "ab") as f:
                    f.write(out)
                    f.flush()
            sys.stdout.buffer.write(out)
            sys.stdout.buffer.flush()
    except OSError as e:
        print("\n--- %s error: %s ---" % (tag, e), flush=True)
    finally:
        conn.close()
        print("\n--- %s disconnected ---" % tag, flush=True)


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(8)
print("listening on 0.0.0.0:%d  ->  %s" % (PORT, log), flush=True)

while True:
    conn, addr = srv.accept()
    # Keepalive so a console that vanishes without a FIN is eventually reaped
    # instead of holding a thread for the life of the process.
    try:
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    except OSError:
        pass
    print("--- connected from %s:%d ---" % addr, flush=True)
    threading.Thread(target=reader, args=(conn, addr), daemon=True).start()

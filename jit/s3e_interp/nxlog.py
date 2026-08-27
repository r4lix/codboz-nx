"""Listen for the NRO's log without netloader.

nxlinkConnectToHost() just opens a TCP connection to __nxlink_host on
NXLINK_CLIENT_PORT and writes stdout/stderr to it -- no handshake, no framing.
So a plain socket server is a complete replacement for `nxlink -s`, and it does
not need the netloader, which is what has been failing (it writes the NRO to
the SD card, and that write errors with EIO).

Put this PC's IPv4 address in sdmc:/switch/boz/nxlink_host.txt, start this,
then launch the NRO from hbmenu.
"""
import socket, sys, time

PORT = 28771            # NXLINK_CLIENT_PORT
log = sys.argv[1] if len(sys.argv) > 1 else "nxlog.txt"

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(1)
print("listening on 0.0.0.0:%d  ->  %s" % (PORT, log), flush=True)

while True:
    conn, addr = srv.accept()
    print("--- connected from %s ---" % (addr[0],), flush=True)
    with open(log, "ab") as f:
        f.write(b"\n=== session %s from %s ===\n"
                % (time.strftime("%H:%M:%S").encode(), addr[0].encode()))
        try:
            while True:
                b = conn.recv(65536)
                if not b:
                    break
                f.write(b)
                f.flush()
                sys.stdout.buffer.write(b)
                sys.stdout.buffer.flush()
        except OSError as e:
            print("\n--- connection error: %s ---" % e, flush=True)
    conn.close()
    print("\n--- disconnected ---", flush=True)

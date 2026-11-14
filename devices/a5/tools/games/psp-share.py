#!/usr/bin/env python3
"""psp-share.py [DIR] [--port N]: PSP games from this computer to the iPad, over the network.

PSP games are hundreds of megabytes, more than the iPad's Linux volume holds:
this serves the disc images (.iso, .cso, .chd) under DIR (default: the
current directory) the way PPSSPP's own "Share games" server does, so PPSSPP
on the iPad streams them -- in its Games tab, "Remote disc streaming", or in
the iPad's Games list when ~/Games/psp-server holds this server's address.
Not .pbp: PPSSPP runs one from its folder, which a URL does not have (1.20.4:
"Could not find executable umd0:/..."); those go into ~/Games/psp.

  GET /            the games, one URL-encoded path a line (PPSSPP's listing)
  HEAD, GET FILE   the file, with byte ranges (PPSSPP reads a disc as it needs)

Only Python's standard library.  Files outside DIR are never served.
"""
import argparse
import os
import socket
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GAMES = (".iso", ".cso", ".chd")


class Share(BaseHTTPRequestHandler):
    root = "."
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def listing(self):
        lines = ["/"]
        for d, dirs, files in os.walk(self.root):
            dirs.sort()
            for f in sorted(files):
                if f.lower().endswith(GAMES):
                    rel = os.path.relpath(os.path.join(d, f), self.root)
                    lines.append("/" + urllib.parse.quote(rel.replace(os.sep, "/")))
        return ("\n".join(lines) + "\n").encode()

    def path_of(self):
        rel = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path).lstrip("/")
        full = os.path.realpath(os.path.join(self.root, rel))
        if not full.startswith(os.path.realpath(self.root) + os.sep) or not os.path.isfile(full) \
                or not full.lower().endswith(GAMES):
            return None
        return full

    def send(self, code, length, ctype="application/octet-stream", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()

    def answer(self, body):
        if self.path in ("/", ""):
            data = self.listing()
            self.send(200, len(data), "text/plain")
            if body:
                self.wfile.write(data)
            return
        full = self.path_of()
        if not full:
            self.send(404, 0, "text/plain")
            return
        size = os.path.getsize(full)
        first, last = 0, size - 1
        rng = self.headers.get("Range", "")
        if rng.startswith("bytes="):
            a, _, b = rng[6:].split(",")[0].partition("-")
            try:
                if a:
                    first, last = int(a), (int(b) if b else size - 1)
                else:                       # the last N bytes
                    first = max(0, size - int(b))
            except ValueError:
                self.send(400, 0, "text/plain")
                return
            last = min(last, size - 1)
            if first > last:
                self.send(416, 0, "text/plain", [("Content-Range", "bytes */%d" % size)])
                return
            self.send(206, last - first + 1, extra=[("Content-Range", "bytes %d-%d/%d" % (first, last, size))])
        else:
            self.send(200, size)
        if not body:
            return
        with open(full, "rb") as f:
            f.seek(first)
            left = last - first + 1
            while left:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def do_HEAD(self):
        self.answer(False)

    def do_GET(self):
        self.answer(True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", nargs="?", default=".")
    ap.add_argument("--port", type=int, default=9123)
    a = ap.parse_args()
    Share.root = os.path.realpath(a.dir)
    games = Share.listing(Share).decode().split("\n")[1:-1]
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Share)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))         # no packet is sent: only the route's source address
        ip = s.getsockname()[0]
    except OSError:
        ip = "THIS-COMPUTER"
    print("%d PSP game(s) in %s" % (len(games), Share.root))
    print("on the iPad: echo http://%s:%d/ > ~/Games/psp-server   (or PPSSPP: Remote disc streaming, %s port %d)"
          % (ip, a.port, ip, a.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

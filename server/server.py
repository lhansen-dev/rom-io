#!/usr/bin/env python3
"""rom-io server: serves a ROM folder over HTTP to the 3DS client.

GET /<dir>/   -> plain-text listing, one entry per line: "name/" for folders, "name\tsize" for files
GET /<file>   -> the file bytes
"""
import argparse
import io
import os
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(SimpleHTTPRequestHandler):
    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            self.log_message("Client disconnected before the response completed")

    # translate_path() from the base class already drops ".." so requests stay inside the root.
    def list_directory(self, path):
        try:
            entries = sorted(os.scandir(path), key=lambda e: (not e.is_dir(), e.name.lower()))
        except OSError:
            self.send_error(404)
            return None
        lines = [e.name + "/" if e.is_dir() else f"{e.name}\t{e.stat().st_size}"
                 for e in entries if not e.name.startswith(".")]
        body = "\n".join(lines).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        return io.BytesIO(body)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", default=Path(__file__).resolve().parent.parent / "roms")
    p.add_argument("--port", type=int, default=8765)
    a = p.parse_args()
    if not Path(a.root).is_dir():
        raise SystemExit(f"ROM folder not found: {a.root}")
    print(f"Serving {a.root} on port {a.port}")
    ThreadingHTTPServer(("0.0.0.0", a.port), partial(Handler, directory=str(a.root))).serve_forever()


if __name__ == "__main__":
    main()

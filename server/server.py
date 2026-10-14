#!/usr/bin/env python3
"""rom-io server: serves a ROM folder over HTTP to the 3DS client.

GET /<dir>/   -> plain-text listing, one entry per line: "name/" for folders, "name\tsize" for files
GET /<file>   -> the file bytes
"""
import argparse
import io
import os
import re
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(SimpleHTTPRequestHandler):
    def send_head(self):
        self._remaining = None
        self._etag = None
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().send_head()
        try:
            source = open(path, "rb")
        except OSError:
            self.send_error(404)
            return None
        stat = os.fstat(source.fileno())
        self._etag = f'"{stat.st_ino:x}-{stat.st_size:x}-{stat.st_mtime_ns:x}"'
        requested = self.headers.get("Range", "") if self.command == "GET" else ""
        if self.headers.get("If-Range", self._etag) != self._etag:
            requested = ""
        match = re.fullmatch(r"bytes=(\d*)-(\d*)", requested.strip())
        if not match or not any(match.groups()):
            source.close()
            return super().send_head()
        try:
            first, last = match.groups()
            if first:
                start = int(first)
                end = min(int(last), stat.st_size - 1) if last else stat.st_size - 1
            else:
                suffix = int(last)
                start, end = max(0, stat.st_size - suffix), stat.st_size - 1
                if suffix == 0:
                    start = stat.st_size
        except ValueError:
            source.close()
            return super().send_head()
        if start >= stat.st_size or end < start:
            source.close()
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{stat.st_size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        source.seek(start)
        self._remaining = end - start + 1
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Length", str(self._remaining))
        self.send_header("Content-Range", f"bytes {start}-{end}/{stat.st_size}")
        self.send_header("Last-Modified", self.date_time_string(stat.st_mtime))
        self.end_headers()
        return source

    def end_headers(self):
        if getattr(self, "_etag", None):
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", self._etag)
        super().end_headers()

    def copyfile(self, source, outputfile):
        if self._remaining is None:
            return super().copyfile(source, outputfile)
        while self._remaining:
            data = source.read(min(self._remaining, 256 * 1024))
            if not data:
                break
            outputfile.write(data)
            self._remaining -= len(data)

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

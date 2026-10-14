import http.client
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path

from server import Handler


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name, "roms")
        (root / "gba").mkdir(parents=True)
        (root / "gba" / "My Game (USA).gba").write_bytes(b"x" * 10)
        (root / "range.bin").write_bytes(bytes(range(256)) * 4096)
        (root / "empty.bin").touch()
        Path(self.tmp.name, "secret.txt").write_text("nope")
        Handler.log_message = lambda *a: None
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(root)))
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.srv.server_port}"

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.tmp.cleanup()

    def get(self, path):
        return urllib.request.urlopen(self.base + path).read()

    def test_listing_and_download(self):
        self.assertEqual(self.get("/"), b"gba/\nempty.bin\t0\nrange.bin\t1048576")
        self.assertEqual(self.get("/gba/"), b"My Game (USA).gba\t10")
        self.assertEqual(self.get("/gba/My%20Game%20%28USA%29.gba"), b"x" * 10)

    def test_no_escape_from_root(self):
        with self.assertRaises(urllib.error.HTTPError):
            self.get("/../secret.txt")
        with self.assertRaises(urllib.error.HTTPError):
            self.get("/gba/%2e%2e/%2e%2e/secret.txt")

    def test_client_disconnect_does_not_raise_server_error(self):
        for error in (ConnectionResetError, BrokenPipeError):
            with self.subTest(error=error.__name__):
                with patch.object(self.srv, "handle_error") as handle_error:
                    with patch.object(Handler, "copyfile", side_effect=error("disconnected")):
                        with self.assertRaises(http.client.IncompleteRead):
                            self.get("/gba/My%20Game%20%28USA%29.gba")
                    handle_error.assert_not_called()
                self.assertEqual(self.get("/gba/My%20Game%20%28USA%29.gba"), b"x" * 10)

    def test_ranges_and_head(self):
        with urllib.request.urlopen(urllib.request.Request(self.base + "/range.bin", method="HEAD")) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.headers["Content-Length"], "1048576")
            self.assertEqual(response.headers["Accept-Ranges"], "bytes")
            etag = response.headers["ETag"]
            self.assertEqual(response.read(), b"")
        expected = bytes(range(256)) * 4096
        for header, start, end in (("bytes=17-300", 17, 300), ("bytes=1048500-", 1048500, 1048575),
                                   ("bytes=-17", 1048559, 1048575), ("bytes=1048500-2000000", 1048500, 1048575)):
            with self.subTest(header=header):
                request = urllib.request.Request(self.base + "/range.bin", headers={"Range": header, "If-Range": etag})
                with urllib.request.urlopen(request) as response:
                    self.assertEqual(response.status, 206)
                    self.assertEqual(response.headers["Content-Range"], f"bytes {start}-{end}/1048576")
                    self.assertEqual(response.read(), expected[start:end + 1])

    def test_unsatisfiable_ranges(self):
        for path, header in (("/range.bin", "bytes=1048576-"), ("/range.bin", "bytes=9-2"),
                             ("/range.bin", "bytes=-0"), ("/empty.bin", "bytes=0-0")):
            with self.subTest(path=path, header=header):
                request = urllib.request.Request(self.base + path, headers={"Range": header})
                with self.assertRaises(urllib.error.HTTPError) as raised:
                    urllib.request.urlopen(request)
                with raised.exception as response:
                    self.assertEqual(response.code, 416)
                    self.assertTrue(response.headers["Content-Range"].startswith("bytes */"))
                    self.assertEqual(response.read(), b"")

    def test_changed_file_or_unsupported_range_returns_full_body(self):
        for headers in ({"Range": "bytes=0-3", "If-Range": '"stale"'},
                        {"Range": "bytes=0-3,8-9"}, {"Range": "invalid"}):
            with self.subTest(headers=headers):
                request = urllib.request.Request(self.base + "/range.bin", headers=headers)
                with urllib.request.urlopen(request) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(len(response.read()), 1048576)

    def test_concurrent_ranges_reconstruct_original(self):
        def fetch(header):
            request = urllib.request.Request(self.base + "/range.bin", headers={"Range": header})
            with urllib.request.urlopen(request) as response:
                self.assertEqual(response.status, 206)
                return response.read()
        for count in (2, 3, 4):
            with self.subTest(streams=count):
                ranges = [f"bytes={1048576 * i // count}-{1048576 * (i + 1) // count - 1}" for i in range(count)]
                with ThreadPoolExecutor(max_workers=count) as executor:
                    parts = list(executor.map(fetch, ranges))
                self.assertEqual(b"".join(parts), bytes(range(256)) * 4096)

    def test_range_beyond_two_gib(self):
        root = Path(self.tmp.name, "roms")
        offset = 3 * 1024 ** 3 + 17
        total = 4 * 1024 ** 3
        with (root / "large.bin").open("wb") as output:
            output.truncate(total) # sparse file: no multi-gigabyte allocation
            output.seek(offset)
            output.write(b"large-file-range")
        request = urllib.request.Request(self.base + "/large.bin", headers={"Range": f"bytes={offset}-{offset + 15}"})
        with urllib.request.urlopen(request) as response:
            self.assertEqual(response.status, 206)
            self.assertEqual(response.headers["Content-Range"], f"bytes {offset}-{offset + 15}/{total}")
            self.assertEqual(response.read(), b"large-file-range")


if __name__ == "__main__":
    unittest.main()

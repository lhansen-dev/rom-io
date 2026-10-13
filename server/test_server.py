import http.client
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request
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
        self.assertEqual(self.get("/"), b"gba/")
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


if __name__ == "__main__":
    unittest.main()

import http.client
import json
import os
import sys
import threading
import unittest
from http.server import ThreadingHTTPServer

import helpers

sys.path.insert(0, os.path.join(helpers.SDK, "studio"))
import server  # noqa: E402
from purrsdk import workspace  # noqa: E402


class Studio(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ws = workspace.Workspace(helpers.tmpdir()).ensure()
        cls.state = server.State(cls.ws)
        cls.httpd = ThreadingHTTPServer(("127.0.0.1", 0), server.make_handler(cls.state))
        cls.port = cls.httpd.server_address[1]
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()

    def call(self, method, path, body=None, token=True, host=None, raw=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=20)
        headers = {"Host": host or "127.0.0.1:%d" % self.port}
        if token:
            headers["Cookie"] = "purr_token=" + self.state.token
        data = raw
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        c.request(method, path, data, headers)
        r = c.getresponse()
        text = r.read()
        try:
            return r.status, json.loads(text)
        except ValueError:
            return r.status, text

    def test_needs_the_token(self):
        self.assertEqual(self.call("GET", "/api/state", token=False)[0], 403)
        self.assertEqual(self.call("POST", "/api/inspect", {"path": "/"}, token=False)[0], 403)

    def test_refuses_other_hosts(self):
        self.assertEqual(self.call("GET", "/api/state", host="evil.example:80")[0], 403)

    def test_state_and_pages(self):
        st, body = self.call("GET", "/api/state")
        self.assertEqual(st, 200)
        self.assertIn("driver", body["kinds"])
        st, html = self.call("GET", "/")
        self.assertEqual(st, 200)
        self.assertIn(b"PURR Studio", html)

    def test_no_path_tricks(self):
        self.assertEqual(self.call("GET", "/../server.py")[0], 404)
        self.assertEqual(self.call("GET", "/api/projects/files?project=../x")[0], 400)

    @unittest.skipUnless(helpers.HAVE_CRYPTO, "needs the cryptography package")
    def test_drop_an_image(self):
        k = helpers.Key()
        data = k.sign_image(helpers.make(), 4)
        st, rep = self.call("POST", "/api/inspect/upload?name=demo.cat", raw=data)
        self.assertEqual(st, 200)
        self.assertEqual(rep["kind"], "purr-image")
        self.assertEqual(rep["file"]["name"], "demo.cat")

    def test_brand_and_boards(self):
        st, r = self.call("POST", "/api/brand/check", {"text": '[brand]\nlogo = "MOON!"\n'})
        self.assertEqual(st, 200)
        self.assertIn("PKG_BRAND_LOGO", r["header"])
        st, r = self.call("GET", "/api/board?path=" + os.path.join(helpers.SDK, "boards", "tdeck_plus.toml"))
        self.assertEqual(st, 200)
        self.assertIn("pinmap", r)
        self.assertEqual(self.call("GET", "/api/board?path=/etc/passwd")[0], 400)

    def test_projects_and_files(self):
        st, r = self.call("POST", "/api/projects/new", {"kind": "driver", "name": "lora-x", "class": "lora"})
        self.assertEqual(st, 200)
        st, r = self.call("GET", "/api/projects/files?project=lora-x")
        self.assertIn("src/lora_x_module.c", r["files"])
        st, r = self.call("GET", "/api/projects/file?project=lora-x&path=../../etc/passwd")
        self.assertEqual(st, 400)
        st, r = self.call("POST", "/api/projects/new", {"kind": "driver", "name": "Bad Name", "class": "lora"})
        self.assertEqual(st, 400)
        self.assertIn("lowercase", r["error"])


if __name__ == "__main__":
    unittest.main()

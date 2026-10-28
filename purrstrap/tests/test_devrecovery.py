"""The dev server hosting recovery images: manifest generation, and a server that cannot be walked out of its folder."""
import hashlib
import http.client
import os
import tempfile
import threading
import unittest

from helpers import make_ctx
from lib import image as pimg
from scripts import dev


def img(name, version, payload=b"payload bytes", kind=pimg.IMG_MODULE, flags=0):
    return pimg.make_image(name, version, kind, flags, payload, pimg.CHIP_ESP32S3)


class RecoveryHosting(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.repo = os.path.join(self.tmp, "repo")
        self.files = {}
        for comp, ext in (("kernel", ".cat"), ("kittenos", ".kitt"), ("loader", ".cat"), ("bootpkg", ".cat")):
            path = os.path.join(self.tmp, comp + ext)
            with open(path, "wb") as fh:
                fh.write(img(comp, "1.2.3", comp.encode() * 10))
            self.files[comp] = path
        self.mod = os.path.join(self.tmp, "about.cat")
        with open(self.mod, "wb") as fh:
            fh.write(img("about", "0.1.1"))
        self.legal = os.path.join(self.tmp, "legal.txt")
        with open(self.legal, "w") as fh:
            fh.write("# note\nCredits.\n")

    def run_recovery(self, **kw):
        ctx, out = make_ctx(self.tmp)
        args = dict(dir=self.repo, board="tdeck_plus", kernel=self.files["kernel"], kittenos=self.files["kittenos"],
                    loader=self.files["loader"], bootpkg=self.files["bootpkg"], legal=self.legal, modules=self.mod)
        args.update(kw)
        return dev.recovery(ctx, **args), out.getvalue()

    def test_writes_both_manifests_in_the_device_format(self):
        rc, _ = self.run_recovery()
        self.assertEqual(rc, 0)
        rdir = os.path.join(self.repo, "recovery")
        text = open(os.path.join(rdir, "recovery.manifest")).read()
        stanzas = [s for s in text.split("\n\n") if "component=" in s]
        self.assertEqual(len(stanzas), 4)
        k = next(s for s in stanzas if "component=kernel" in s)
        data = open(os.path.join(rdir, "kernel-tdeck_plus.cat"), "rb").read()
        for want in ("version=1.2.3", "chip=esp32s3", "board=tdeck_plus", "file=kernel-tdeck_plus.cat",
                     "size=%d" % len(data), "sha256=" + hashlib.sha256(data).hexdigest(), "key=boot"):
            self.assertIn(want, k)
        self.assertIn("file=kittenos-tdeck_plus.kitt", text)
        mods = open(os.path.join(rdir, "modules.manifest")).read()
        self.assertIn("component=about", mods)
        self.assertIn("type=module", mods)
        self.assertNotIn("component=kernel", mods)             # components and modules stay in their own manifests
        self.assertTrue(os.path.exists(os.path.join(rdir, "legal.txt")))

    def test_refuses_what_is_not_an_image_or_a_bad_module_name(self):
        junk = os.path.join(self.tmp, "junk.cat")
        with open(junk, "wb") as fh:
            fh.write(b"not an image at all" * 20)
        rc, out = self.run_recovery(kernel=junk)
        self.assertEqual(rc, 1)
        clash = os.path.join(self.tmp, "clash.cat")
        with open(clash, "wb") as fh:
            fh.write(img("kernel", "9.9.9"))                    # a module may not take a component's name
        rc, _ = self.run_recovery(modules=clash)
        self.assertEqual(rc, 1)

    def test_safe_names(self):
        for ok in ("recovery.manifest", "kernel-tdeck_plus.cat", "legal.txt"):
            self.assertTrue(dev._safe_recovery_name(ok), ok)
        for bad in ("../x", "a/b", "..", "a..b", ".hidden", "", "x" * 81, "a%2fb", "a\\b", "a b"):
            self.assertFalse(dev._safe_recovery_name(bad), repr(bad))

    def test_the_server_serves_the_folder_and_nothing_else(self):
        self.run_recovery()
        with open(os.path.join(self.repo, "secret.txt"), "w") as fh:
            fh.write("not for the device")
        dev._Handler.repo, dev._Handler.name, dev._Handler.repo_id, dev._Handler.quiet = self.repo, "t", "abc", True
        srv = dev._Server(("127.0.0.1", 0), dev._Handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            def get(path):
                c = http.client.HTTPConnection("127.0.0.1", srv.server_address[1], timeout=5)
                c.request("GET", path)
                r = c.getresponse()
                return r.status, r.read()
            st, body = get("/recovery/recovery.manifest")
            self.assertEqual(st, 200)
            self.assertIn(b"component=kernel", body)
            st, body = get("/recovery/legal.txt")
            self.assertEqual((st, body), (200, b"# note\nCredits.\n"))
            self.assertEqual(get("/recovery/kernel-tdeck_plus.cat")[0], 200)
            for bad in ("/recovery/../secret.txt", "/recovery/..%2fsecret.txt", "/recovery/", "/recovery/missing.cat",
                        "/recovery/sub/x"):
                self.assertEqual(get(bad)[0], 404, bad)
        finally:
            srv.shutdown()


if __name__ == "__main__":
    unittest.main()


class Announcing(unittest.TestCase):
    def test_the_server_announces_itself_and_find_hears_it(self):
        import socket
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        rx.bind(("127.0.0.1", 0))
        rx.settimeout(3)
        port = rx.getsockname()[1]
        stop = threading.Event()
        t = threading.Thread(target=dev._announce_loop, args=("My Mac", 47801, "abc123def456", stop, 0.2),
                             kwargs={"targets": ["127.0.0.1"], "discovery_port": port}, daemon=True)
        t.start()
        try:
            data, _ = rx.recvfrom(128)
            self.assertEqual(data, b"PURR-REPO! My_Mac 47801 abc123def456")          # the format the device parses
            data, _ = rx.recvfrom(128)                                                 # and it repeats
            self.assertEqual(data, b"PURR-REPO! My_Mac 47801 abc123def456")
        finally:
            stop.set()
            rx.close()

    def test_broadcast_addresses_always_include_the_limited_broadcast(self):
        addrs = dev._broadcast_addresses()
        self.assertEqual(addrs[0], "255.255.255.255")
        self.assertEqual(len(addrs), len(set(addrs)))

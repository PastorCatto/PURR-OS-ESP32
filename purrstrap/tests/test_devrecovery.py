"""The dev server hosting recovery images: manifest generation, and a server that cannot be walked out of its folder."""
import hashlib
import http.client
import os
import re
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
        sha = hashlib.sha256(data).hexdigest()
        for want in ("version=1.2.3", "chip=esp32s3", "board=tdeck_plus", "file=kernel-tdeck_plus.%s.cat" % sha[:10],
                     "size=%d" % len(data), "sha256=" + sha, "key=boot"):
            self.assertIn(want, k)
        self.assertRegex(text, r"file=kittenos-tdeck_plus\.[0-9a-f]{10}\.kitt")        # named by its hash: the same name is always the same bytes
        self.assertEqual(open(os.path.join(rdir, "kernel-tdeck_plus.%s.cat" % sha[:10]), "rb").read(), data)
        mods = open(os.path.join(rdir, "modules.manifest")).read()
        self.assertIn("component=about", mods)
        self.assertIn("type=module", mods)
        self.assertNotIn("component=kernel", mods)             # components and modules stay in their own manifests
        self.assertTrue(os.path.exists(os.path.join(rdir, "legal.txt")))

    def named_kernel(self):
        text = open(os.path.join(self.repo, "recovery", "recovery.manifest")).read()
        return next(l.split("=", 1)[1] for l in text.splitlines() if l.startswith("file=kernel-"))

    def test_a_republish_never_changes_what_an_older_manifest_names(self):
        # A device that has read the manifest and is still downloading must find the same bytes: the images are named by their hash, the old ones stay
        # for a few generations, and the manifest is replaced last.
        self.run_recovery()
        rdir = os.path.join(self.repo, "recovery")
        old_name = self.named_kernel()
        old_bytes = open(os.path.join(rdir, old_name), "rb").read()
        with open(self.files["kernel"], "wb") as fh:
            fh.write(img("kernel", "1.2.4", b"a different kernel" * 10))
        rc, _ = self.run_recovery()
        self.assertEqual(rc, 0)
        new_name = self.named_kernel()
        self.assertNotEqual(old_name, new_name)
        self.assertEqual(open(os.path.join(rdir, old_name), "rb").read(), old_bytes)         # the first manifest's file is still there, unchanged
        self.assertTrue(os.path.exists(os.path.join(rdir, new_name)))
        self.assertEqual([f for f in os.listdir(rdir) if ".tmp-" in f], [])                   # nothing half written is left behind

    def test_old_generations_are_pruned_but_a_few_stay(self):
        rdir = os.path.join(self.repo, "recovery")
        names = []
        for i in range(5):
            with open(self.files["kernel"], "wb") as fh:
                fh.write(img("kernel", "1.2.%d" % (10 + i), b"kernel number %d" % i * 4))
            self.run_recovery()
            names.append(self.named_kernel())
            os.utime(os.path.join(rdir, names[-1]), (1000 + i, 1000 + i))                     # distinct ages for the pruning
        left = [f for f in os.listdir(rdir) if re.match(r"^kernel-tdeck_plus\.[0-9a-f]{10}\.cat$", f)]
        self.assertEqual(len(left), dev.KEEP_GENERATIONS)
        self.assertIn(names[-1], left)                                                       # the newest is always kept
        self.assertNotIn(names[0], left)

    def test_an_image_bigger_than_its_slot_is_refused(self):
        big = os.path.join(self.tmp, "kittenos-big.kitt")
        with open(big, "wb") as fh:
            fh.write(img("kittenos", "1.2.4", b"x" * (0x1C0000 + 100), kind=pimg.IMG_RECOVERY))
        rc, out = self.run_recovery(kittenos=big)
        self.assertEqual(rc, 1)
        self.assertIn("too many", out)
        self.assertFalse(os.path.exists(os.path.join(self.repo, "recovery", "kittenos-tdeck_plus.kitt")))

    def test_withdraw_marks_the_manifests_and_unwithdraw_takes_it_back(self):
        self.run_recovery()
        rdir = os.path.join(self.repo, "recovery")
        ctx, out = make_ctx(self.tmp)
        self.assertEqual(dev.withdraw(ctx, self.repo, "1.2.3", "kernel,kittenos", "wizard showed two Wi-Fi pages"), 0)
        text = open(os.path.join(rdir, "recovery.manifest")).read()
        self.assertIn("withdrawn_versions=kernel@1.2.3,kittenos@1.2.3", text)
        self.assertIn("withdrawn_reason=wizard showed two Wi-Fi pages", text)
        k = next(x for x in text.split("\n\n") if "component=kernel" in x)
        l = next(x for x in text.split("\n\n") if "component=loader" in x)
        self.assertIn("withdrawn=yes", k)                       # the entry says so too
        self.assertNotIn("withdrawn=yes", l)                    # the loader was not named
        self.assertIn("still hosts", out.getvalue())            # the tool says what the server still offers
        self.assertIn("withdrawn_versions=", open(os.path.join(rdir, "modules.manifest")).read())
        # the list survives the next publish: a new `dev recovery` writes it again
        self.run_recovery()
        self.assertIn("withdrawn_versions=kernel@1.2.3,kittenos@1.2.3", open(os.path.join(rdir, "recovery.manifest")).read())
        self.assertEqual(dev.unwithdraw(ctx, self.repo, "1.2.3", "kernel,kittenos"), 0)
        text = open(os.path.join(rdir, "recovery.manifest")).read()
        self.assertNotIn("withdrawn", text)
        self.assertFalse(os.path.exists(os.path.join(rdir, "withdrawn.list")))

    def test_withdraw_refuses_a_bad_version_or_no_reason(self):
        self.run_recovery()
        ctx, _ = make_ctx(self.tmp)
        self.assertEqual(dev.withdraw(ctx, self.repo, "latest", "*", "bad"), 1)
        self.assertEqual(dev.withdraw(ctx, self.repo, "1.2.3", "*", ""), 1)
        self.assertEqual(dev.withdraw(ctx, self.repo, "1.2.3", "ker nel", "bad"), 1)
        self.assertEqual(dev.unwithdraw(ctx, self.repo, "9.9.9", "*"), 1)

    def test_a_ui_pack_is_a_component_signed_for_the_system_key(self):
        pack = os.path.join(self.tmp, "uiassets.cat")
        with open(pack, "wb") as fh:
            fh.write(img("uiassets", "1.0.17", b"pack bytes" * 30, flags=pimg.MOD_UIASSETS))
        rc, _ = self.run_recovery(uiassets=pack)
        self.assertEqual(rc, 0)
        rdir = os.path.join(self.repo, "recovery")
        self.assertTrue(os.path.exists(os.path.join(rdir, "uiassets-tdeck_plus.cat")))
        text = open(os.path.join(rdir, "recovery.manifest")).read()
        stanza = next(s for s in text.split("\n\n") if "component=uiassets" in s)
        for want in ("version=1.0.17", "key=system", "payload_size=300"):
            self.assertIn(want, stanza)
        self.assertRegex(stanza, r"file=uiassets-tdeck_plus\.[0-9a-f]{10}\.cat")
        self.assertNotIn("component=uiassets", open(os.path.join(rdir, "modules.manifest")).read())
        self.assertEqual(len([s for s in text.split("\n\n") if "component=" in s]), 5)

    def test_the_two_per_shell_ui_packs_are_components_signed_for_the_system_key(self):
        for comp in ("ui_w10m", "ui_android"):
            pack = os.path.join(self.tmp, comp + ".cat")
            with open(pack, "wb") as fh:
                fh.write(img(comp, "1.0.29", comp.encode() * 40, flags=pimg.MOD_UIASSETS))
            rc, _ = self.run_recovery(**{comp: pack})
            self.assertEqual(rc, 0, comp)
        rdir = os.path.join(self.repo, "recovery")
        text = open(os.path.join(rdir, "recovery.manifest")).read()
        modules = open(os.path.join(rdir, "modules.manifest")).read()
        for comp, size in (("ui_w10m", 7 * 40), ("ui_android", 10 * 40)):
            self.assertTrue(os.path.exists(os.path.join(rdir, "%s-tdeck_plus.cat" % comp)))
            stanza = next(s for s in text.split("\n\n") if "component=%s\n" % comp in s + "\n")
            for want in ("version=1.0.29", "key=system", "payload_size=%d" % size, "board=tdeck_plus"):
                self.assertIn(want, stanza)
            self.assertRegex(stanza, r"file=%s-tdeck_plus\.[0-9a-f]{10}\.cat" % comp)
            self.assertNotIn("component=%s" % comp, modules)          # a component, not a module
        self.assertEqual(len([s for s in text.split("\n\n") if "component=" in s]), 6)
        self.assertIn("ui_w10m", dev.RECOVERY_COMPONENTS)
        self.assertIn("ui_android", dev.RECOVERY_COMPONENTS)

    def test_a_ui_pack_is_capped_at_5_mb_instead_of_a_slot(self):
        limit = 5 * 1024 * 1024
        self.assertEqual(dev.SLOT_BYTES["ui_w10m"], limit)
        self.assertEqual(dev.SLOT_BYTES["ui_android"], limit)
        for comp in ("ui_w10m", "ui_android"):
            ok = os.path.join(self.tmp, comp + "-ok.cat")
            with open(ok, "wb") as fh:
                fh.write(img(comp, "1.0.29", b"\x00" * limit, flags=pimg.MOD_UIASSETS))
            rc, out = self.run_recovery(**{comp: ok})
            self.assertEqual(rc, 0, "exactly the cap is allowed")
            big = os.path.join(self.tmp, comp + "-big.cat")
            with open(big, "wb") as fh:
                fh.write(img(comp, "1.0.29", b"\x00" * (limit + 1), flags=pimg.MOD_UIASSETS))
            rc, out = self.run_recovery(**{comp: big})
            self.assertEqual(rc, 1)
            self.assertIn("1 too many", out)
            self.assertIn("UI pack", out)

    def test_a_module_cannot_take_a_ui_pack_name(self):
        for comp in ("ui_w10m", "ui_android"):
            clash = os.path.join(self.tmp, "clash-%s.cat" % comp)
            with open(clash, "wb") as fh:
                fh.write(img(comp, "1.0.0"))
            rc, out = self.run_recovery(modules=clash)
            self.assertEqual(rc, 1)
            self.assertIn("cannot be called", out)

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

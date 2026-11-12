"""Studio's device image routes (image/boards, image/folders, image/plan) over the real HTTP handler."""
import os
import unittest

import helpers
import test_studio


class ImageRoutes(test_studio.Studio):
    def test_boards(self):
        st, body = self.call("GET", "/api/image/boards")
        self.assertEqual(st, 200)
        self.assertEqual([b["id"] for b in body["boards"]], ["tdeck_plus", "tdeck", "waveshare154", "playtime"])

    def test_plan_of_an_empty_folder_lists_what_is_missing(self):
        folder = helpers.tmpdir()
        st, body = self.call("GET", "/api/image/plan?board=tdeck_plus&folder=" + folder)
        self.assertEqual(st, 200)
        self.assertFalse(body["ok"])
        self.assertEqual(len(body["parts"]), 5)
        self.assertTrue(all(not p["found"] for p in body["parts"]))

    def test_unknown_board_is_a_plain_error(self):
        st, body = self.call("GET", "/api/image/plan?board=nope&folder=" + os.getcwd())
        self.assertEqual(st, 400)
        self.assertIn("partition table", body["error"])

    def test_folders_offers_the_kit_images(self):
        st, body = self.call("GET", "/api/image/folders?board=tdeck_plus")
        self.assertEqual(st, 200)
        self.assertIn("folders", body)


if __name__ == "__main__":
    unittest.main()

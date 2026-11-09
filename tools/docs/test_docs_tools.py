"""Tests for the documentation tools: python3 -m unittest discover -s tools/docs"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_links  # noqa: E402
import count_words  # noqa: E402
import mdscan  # noqa: E402


def tree(files):
    d = tempfile.mkdtemp(prefix="docs-test-")
    for name, text in files.items():
        p = os.path.join(d, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(text)
    return d


class Slugs(unittest.TestCase):
    def test_github_style(self):
        self.assertEqual(mdscan.slug("What it is for"), "what-it-is-for")
        self.assertEqual(mdscan.slug("The `ui` catcall (v5)"), "the-ui-catcall-v5")
        self.assertEqual(mdscan.slug("A, B & C: done!"), "a-b--c-done")

    def test_repeated_headings_get_numbers(self):
        a = mdscan.anchors("# Intro\n\n## Notes\n\n## Notes\n\n## Notes\n")
        self.assertTrue({"intro", "notes", "notes-1", "notes-2"} <= a)

    def test_headings_in_code_are_not_anchors(self):
        self.assertNotIn("fake", mdscan.anchors("```\n# fake\n```\n# real\n"))


class Links(unittest.TestCase):
    def test_code_is_ignored(self):
        text = "see [a](a.md)\n`[b](b.md)`\n```\n[c](c.md)\n```\n![img](pic.png)\n"
        self.assertEqual([t for _, t in mdscan.links(text)], ["a.md", "pic.png"])

    def test_reference_definitions(self):
        self.assertEqual([t for _, t in mdscan.links("[x][1]\n\n[1]: other.md \"Title\"\n")], ["other.md"])

    def test_a_good_tree_is_clean_and_a_bad_one_is_reported(self):
        d = tree({"a.md": "# A\n\n[b](b.md#second) [self](#a) [web](https://example.org) [up](sub/c.md)\n",
                  "b.md": "# B\n\n## Second\n", "sub/c.md": "[back](../a.md)\n"})
        problems, nfiles, nlinks, nweb = check_links.check([d], root=d)
        self.assertEqual((problems, nfiles, nlinks, nweb), ([], 3, 4, 1))
        d2 = tree({"a.md": "[gone](nope.md)\n[bad anchor](b.md#missing)\n", "b.md": "# B\n"})
        problems, *_ = check_links.check([d2], root=d2)
        self.assertEqual([(p[1], p[3].split(" in ")[0]) for p in problems], [(1, "no such file"), (2, "no such heading")])

    def test_a_percent_encoded_path_resolves(self):
        d = tree({"a.md": "[x](my%20file.md)\n", "my file.md": "# X\n"})
        self.assertEqual(check_links.check([d], root=d)[0], [])


class Words(unittest.TestCase):
    def test_prose_counts_and_the_rest_does_not(self):
        text = "# A heading here\n\nOne two three.\n\n| a | b |\n|---|---|\n| x | y |\n\n```\ncode code code\n```\n\n- a [link text](x.md) here\n"
        self.assertEqual(count_words.words(text), 3 + 3 + 4)


if __name__ == "__main__":
    unittest.main()

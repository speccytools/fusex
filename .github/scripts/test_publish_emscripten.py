#!/usr/bin/env python3
"""Offline tests for web publication (no Emscripten or GitHub account needed)."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "publisher", Path(__file__).with_name("publish_emscripten.py")
)
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)


class PublicationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.sources = Path(self.temp.name) / "sources"
        self.site = Path(self.temp.name) / "site"
        self.site.mkdir()
        for project in ("fuse", "libspectrum"):
            root = self.sources / project
            root.mkdir(parents=True)
            (root / "COPYING").write_text("Test licence\n")
            if project == "fuse":
                (root / "roms").mkdir()
                (root / "roms/README.copyright").write_text("Test ROM notice\n")
            for args in (
                ["init", "-q"], ["add", "."],
                ["-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "commit", "-qm", "Fixture"],
            ):
                subprocess.run(["git", "-C", str(root), *args], check=True)
        self.build = self.sources / "build/wasm/fuse"
        self.build.mkdir(parents=True)
        for name in ("fuse.html", "fuse.js", "fuse.wasm", "fuse.data"):
            (self.build / name).write_text(name)

    def publish(self, fuse="master", libspectrum="master"):
        publisher.publish(self.sources, self.site, fuse, libspectrum, "6.0.12")

    def test_master_replaced_releases_preserved(self):
        self.publish("fuse-1.9.2", "libspectrum-1.5.0")
        release = self.site / "releases/fuse-1.9.2/libspectrum-1.5.0"
        original = (release / "build-info.json").read_bytes()
        self.publish()
        (self.build / "fuse.js").write_text("updated")
        self.publish()
        self.assertEqual((self.site / "master/fuse.js").read_text(), "updated")
        self.assertEqual((release / "build-info.json").read_bytes(), original)
        self.assertEqual(len(json.loads((self.site / "builds.json").read_text())), 2)
        self.assertTrue((self.site / "master/index.html").is_file())
        self.assertFalse((self.site / "master/fuse.html").exists())
        self.assertTrue((release / "notices/roms-copyright.txt").is_file())

    def test_release_cannot_be_overwritten(self):
        self.publish("v1", "v2")
        before = (self.site / "builds.json").read_bytes()
        with self.assertRaisesRegex(ValueError, "already published"):
            self.publish("v1", "v2")
        self.assertEqual((self.site / "builds.json").read_bytes(), before)

    def test_tags_with_slashes_and_html_characters(self):
        self.publish("release/v1&test", "v2")
        self.assertTrue((self.site / "releases/release%2Fv1%26test/v2/index.html").is_file())
        index = (self.site / "index.html").read_text()
        self.assertIn("release%252Fv1%2526test/v2/", index)
        self.assertIn("release/v1&amp;test", index)

    def test_reject_mixed_selection(self):
        with self.assertRaises(ValueError):
            self.publish("master", "v1")
        self.assertEqual(list(self.site.iterdir()), [])

    def test_missing_output_leaves_master_untouched(self):
        self.publish()
        before = (self.site / "master/build-info.json").read_bytes()
        (self.build / "fuse.wasm").unlink()
        with self.assertRaisesRegex(ValueError, "Missing or empty"):
            self.publish()
        self.assertEqual((self.site / "master/build-info.json").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()

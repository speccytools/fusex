#!/usr/bin/env python3
"""Host-independent tests for native Windows DLL dependency staging."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "data/win32/check_win32_dll_deps.sh"


class DllStagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / "distribution"
        self.root.mkdir()
        self.prefix = self.base / "mingw32"
        self.bin = self.prefix / "bin"
        self.bin.mkdir(parents=True)
        tools = self.base / "tools"
        tools.mkdir()
        mock = tools / "objdump"
        mock.write_text(
            f"#!{sys.executable}\n"
            "import pathlib, sys\n"
            "text = pathlib.Path(sys.argv[-1]).read_text()\n"
            "if text == 'INVALID': sys.exit(1)\n"
            "print(text)\n"
        )
        mock.chmod(0o755)
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                        MINGW_PREFIX=str(self.prefix), WIN32_DLL_SEARCH_DIRS="")

    def run_staging(self):
        return subprocess.run(["bash", str(SCRIPT), str(self.root)], env=self.env,
                              capture_output=True, text=True)

    def test_recursively_copies_dependencies(self):
        (self.root / "fuse.exe").write_text("DLL Name: first.dll\nDLL Name: KERNEL32.dll\n")
        (self.bin / "first.dll").write_text("DLL Name: second.dll\n")
        (self.bin / "second.dll").write_text("DLL Name: msvcrt.dll\n")
        result = self.run_staging()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "second.dll").exists())
        self.assertFalse((self.root / "KERNEL32.dll").exists())

    def test_preserves_staged_runtime(self):
        (self.root / "fuse.exe").write_text("DLL Name: first.dll\n")
        (self.root / "first.dll").write_text("staged")
        (self.bin / "first.dll").write_text("different")
        self.assertEqual(self.run_staging().returncode, 0)
        self.assertEqual((self.root / "first.dll").read_text(), "staged")

    def test_missing_dependency_fails(self):
        (self.root / "fuse.exe").write_text("DLL Name: absent.dll\n")
        result = self.run_staging()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("absent.dll", result.stderr)

    def test_posix_runtime_is_rejected(self):
        for runtime in ("msys-2.0.dll", "cygwin1.dll"):
            with self.subTest(runtime=runtime):
                (self.root / "fuse.exe").write_text("DLL Name: " + runtime + "\n")
                self.assertNotEqual(self.run_staging().returncode, 0)

    def test_bad_binary_fails(self):
        (self.root / "fuse.exe").write_text("INVALID")
        self.assertNotEqual(self.run_staging().returncode, 0)


class CryptoPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.cache = self.base / "cache"
        self.cache.mkdir()
        source = SCRIPT.parents[2] / ".github/scripts/install_windows_crypto.sh"
        self.script = self.base / source.name
        shutil.copyfile(source, self.script)
        self.name = "mingw-w64-i686-test-1-any.pkg.tar.zst"
        (self.cache / self.name).write_bytes(b"package contents")
        (self.cache / (self.name + ".sig")).write_bytes(b"test signature")
        digest = hashlib.sha256(b"package contents").hexdigest()
        (self.base / "windows-mingw32-crypto.sha256").write_text(
            f"# Test lock\n{digest}  {self.name}\n")
        self.log = self.base / "calls"
        tools = self.base / "tools"
        tools.mkdir()
        for tool in ("pacman", "gpg", "curl"):
            mock = tools / tool
            mock.write_text(
                f"#!{sys.executable}\n"
                "import json, os, pathlib, sys\n"
                "name = pathlib.Path(sys.argv[0]).name\n"
                "with open(os.environ['TEST_CALLS'], 'a') as f:\n"
                "    f.write(json.dumps([name] + sys.argv[1:]) + '\\n')\n"
                "if name == 'curl': sys.exit(99)  # Cached tests never use the network.\n"
                "if name == 'gpg':\n"
                "    if os.environ.get('BAD_SIGNATURE'): sys.exit(1)\n"
                "    trust = 'UNDEFINED' if os.environ.get('UNTRUSTED_SIGNATURE') else 'FULLY'\n"
                "    print('[GNUPG:] TRUST_' + trust + ' 0 pgp')\n"
            )
            mock.chmod(0o755)
        # MSYS2 provides sha256sum; use the host's equivalent in these tests.
        checksum = tools / "sha256sum"
        checksum.write_text("#!/bin/sh\nexec shasum -a 256 \"$@\"\n")
        checksum.chmod(0o755)
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                        MSYSTEM="MINGW32", TEST_CALLS=str(self.log))

    def run_install(self):
        return subprocess.run(["bash", str(self.script), str(self.cache)],
                              env=self.env, capture_output=True, text=True)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_reuses_cached_packages_and_verifies_before_install(self):
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call[0] for call in self.calls()], ["gpg", "pacman"])
        self.assertIn("--verify", self.calls()[0])
        self.assertIn("/etc/pacman.d/gnupg", self.calls()[0])
        self.assertIn(str(self.cache / self.name), self.calls()[-1])

    def test_crlf_lock_reuses_cached_packages(self):
        lock = self.base / "windows-mingw32-crypto.sha256"
        lock.write_bytes(lock.read_bytes().replace(b"\n", b"\r\n"))
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([call[0] for call in self.calls()], ["gpg", "pacman"])

    def test_corrupt_package_is_not_installed(self):
        (self.cache / self.name).write_bytes(b"corrupt")
        self.assertNotEqual(self.run_install().returncode, 0)
        self.assertNotIn("pacman", [call[0] for call in self.calls()])

    def test_bad_signature_is_not_installed(self):
        self.env["BAD_SIGNATURE"] = "1"
        self.assertNotEqual(self.run_install().returncode, 0)
        self.assertNotIn("pacman", [call[0] for call in self.calls()])

    def test_valid_but_untrusted_signature_is_not_installed(self):
        self.env["UNTRUSTED_SIGNATURE"] = "1"
        self.assertNotEqual(self.run_install().returncode, 0)
        self.assertNotIn("pacman", [call[0] for call in self.calls()])

    def test_wrong_environment_is_rejected(self):
        self.env["MSYSTEM"] = "UCRT64"
        self.assertNotEqual(self.run_install().returncode, 0)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()

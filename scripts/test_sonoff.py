"""Exercise the firmware's button debounce without serial hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ButtonTests(unittest.TestCase):
    def test_short_release_long_pair_bounce_boot_held_and_clock_rollover(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Host C++ compiler unavailable")
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "button-test.exe"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "main"), str(root / "scripts/button_core_test.cpp"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

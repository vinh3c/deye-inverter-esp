"""Compile and exercise the actual firmware form parser without hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class LoggerWebTests(unittest.TestCase):
    def test_form_validation_encoding_duplicates_token_and_bounds(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Host C++ compiler unavailable")
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "logger-form-test.exe"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root / "main"), str(root / "scripts/logger_form_core_test.cpp"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable), str(root / "main/logger_config.html")], check=True)

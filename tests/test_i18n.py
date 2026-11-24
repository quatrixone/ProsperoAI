import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class I18nTests(unittest.TestCase):
    def test_catalog_cases(self):
        with tempfile.TemporaryDirectory(prefix="prospero-i18n-") as directory:
            binary = Path(directory) / "i18n-test"
            compiler = os.environ.get("HOST_CXX", "clang++")
            subprocess.run([
                compiler, "-std=c++20", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=undefined", "-UNDEBUG", "-I" + str(ROOT / "src"),
                str(ROOT / "tests/i18n_test.cpp"),
                str(ROOT / "src/i18n.cpp"),
                str(ROOT / "src/i18n_it.cpp"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


class CheckToolTests(unittest.TestCase):
    TOOL = ROOT / "tools/i18n-check.py"

    def run_tool(self, uses, table):
        with tempfile.TemporaryDirectory(prefix="prospero-i18n-check-") as directory:
            app = Path(directory)
            (app / "src").mkdir()
            (app / "src/screen.cpp").write_text(uses)
            rows = "".join('{"%s", "%s", "%s"},\n' % row for row in table)
            (app / "src/i18n_it.cpp").write_text("const Entry kTable[] = {\n" + rows + "};\n")
            return subprocess.run(["python3", "-I", str(self.TOOL), str(app)],
                                  capture_output=True, text=True)

    def test_check_tool_flags_missing_and_unused_ids(self):
        good = self.run_tool('TR("a.one", "One %d"); TR("a.one", "One %d");',
                             [("a.one", "One %d", "Uno %d")])
        self.assertEqual(good.returncode, 0, good.stdout)
        missing = self.run_tool('TR("a.one", "One"); TR("a.two", "Two");', [("a.one", "One", "Uno")])
        self.assertEqual(missing.returncode, 1)
        self.assertIn("missing in it: a.two", missing.stdout)
        unused = self.run_tool('TR("a.one", "One");',
                               [("a.one", "One", "Uno"), ("a.old", "Old", "Vecchio")])
        self.assertEqual(unused.returncode, 1)
        self.assertIn("unused table id: a.old", unused.stdout)
        clash = self.run_tool('TR("a.one", "One"); TR("a.one", "1");', [("a.one", "One", "Uno")])
        self.assertEqual(clash.returncode, 1)
        self.assertIn("conflicting english for a.one", clash.stdout)
        holder = self.run_tool('TR("a.one", "One %d");', [("a.one", "One %d", "Uno %s")])
        self.assertEqual(holder.returncode, 1)
        self.assertIn("placeholder mismatch: a.one", holder.stdout)
        for english, italiano in (("%.2f GiB", "GiB"), ("%.1f of %d", "%d di %.1f"),
                                  ("%.1f", "%.2f"), ("%d%%", "%d")):
            result = self.run_tool('TR("a.one", "%s");' % english, [("a.one", english, italiano)])
            self.assertEqual(result.returncode, 1, (english, italiano))
            self.assertIn("placeholder mismatch: a.one", result.stdout)
        ok = self.run_tool('TR("a.one", "%.2f of %5.1f");', [("a.one", "%.2f of %5.1f", "%.2f su %5.1f")])
        self.assertEqual(ok.returncode, 0, ok.stdout)

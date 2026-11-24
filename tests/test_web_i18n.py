# SPDX-License-Identifier: GPL-3.0-or-later
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WebCatalogTests(unittest.TestCase):
    def test_catalog_cases(self):
        result = subprocess.run(["node", str(ROOT / "tests/web_i18n_test.js")],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_italian_table_cases(self):
        result = subprocess.run(["node", str(ROOT / "tests/web_i18n_test.js"), "--real-table"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class WebCheckToolTests(unittest.TestCase):
    TOOL = ROOT / "tools/i18n-check-web.py"

    def run_tool(self, html, js, table):
        with tempfile.TemporaryDirectory(prefix="prospero-web-i18n-") as directory:
            web = Path(directory) / "assets/web"
            web.mkdir(parents=True)
            (web / "index.html").write_text(html)
            (web / "app.js").write_text(js)
            rows = "".join('  "%s": "%s",\n' % row for row in table)
            (web / "i18n-it.js").write_text('globalThis.i18n.register("it", {\n' + rows + "});\n")
            return subprocess.run(["python3", "-I", str(self.TOOL), directory],
                                  capture_output=True, text=True)

    def test_consistent_files_pass(self):
        result = self.run_tool(
            '<button data-i18n="tab.settings">Settings</button>'
            '<input data-i18n-attr="placeholder:models.search" placeholder="Search models">',
            'status(i18n.t("status.done", "Done {0}", n));',
            [("tab.settings", "Impostazioni"), ("models.search", "Cerca"),
             ("status.done", "Fatto {0}")])
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_missing_id_fails(self):
        result = self.run_tool('<b data-i18n="a.b">Hi</b>', "", [])
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in it: a.b", result.stdout)

    def test_unused_id_fails(self):
        result = self.run_tool('<b data-i18n="a.b">Hi</b>', "", [("a.b", "Ciao"), ("c.d", "Boh")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("unused table id: c.d", result.stdout)

    def test_placeholder_mismatch_fails(self):
        result = self.run_tool("", 'i18n.t("s.x", "Done {0}", n)', [("s.x", "Fatto")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("placeholder mismatch: s.x", result.stdout)

    def test_edge_whitespace_mismatch_fails(self):
        result = self.run_tool("", 'i18n.t("s.x", " GiB. ", n)', [("s.x", "GiB.")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("edge mismatch: s.x", result.stdout)

    def test_final_punctuation_mismatch_fails(self):
        result = self.run_tool("", 'i18n.t("s.x", "Done.", n)', [("s.x", "Fatto")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("edge mismatch: s.x", result.stdout)

    def test_html_text_is_compared_unstripped(self):
        result = self.run_tool('<b data-i18n="a.b"> Hi</b>', "", [("a.b", "Ciao")])
        self.assertEqual(result.returncode, 1)
        self.assertIn("edge mismatch: a.b", result.stdout)

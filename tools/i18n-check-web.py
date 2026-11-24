#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release check for the web string catalog (not a unit test).

Usage: i18n-check-web.py [app_dir]

Collects data-i18n / data-i18n-attr from assets/web/index.html and
i18n.t("id", "english") calls from the other scripts in assets/web, and checks
them against assets/web/i18n-it.js. Exit 1 when a used ID has no Italian entry,
a table ID is unused, two uses of one ID disagree on the English text, an
English/Italian pair has different placeholders, leading/trailing spaces or
final punctuation (. : ? ! …), or the table repeats an ID. Calls are read only
when the id and English text are plain quoted literals (no backticks).
"""
import argparse
import re
import sys
from pathlib import Path

TEXT = re.compile(r'<([a-zA-Z0-9]+)\b([^>]*?)\sdata-i18n="([a-z0-9_.]+)"([^>]*)>([^<]*)</\1>')
TAG = re.compile(r'<[a-zA-Z0-9]+\b[^>]*\sdata-i18n-attr="([^"]+)"[^>]*>')
CALL = re.compile(r'\bi18n\.t\(\s*([\'"])([a-z0-9_.]+)\1\s*,\s*(?:"((?:[^"\\]|\\.)*)"|\'((?:[^\'\\]|\\.)*)\')')
ENTRY = re.compile(r'[\'"]([a-z0-9_.]+)[\'"]\s*:\s*(?:"((?:[^"\\]|\\.)*)"|\'((?:[^\'\\]|\\.)*)\')')
HOLDER = re.compile(r'\{\d+\}|%s')


def collect_uses(web):
    uses = []
    html = (web / "index.html").read_text(encoding="utf-8")
    for m in TEXT.finditer(html):
        uses.append((m.group(3), m.group(5), "index.html"))
    for tag in TAG.finditer(html):
        for pair in tag.group(1).split(";"):
            name, _, tid = (part.strip() for part in pair.partition(":"))
            value = re.search(r'\s' + re.escape(name) + r'="([^"]*)"', tag.group(0))
            uses.append((tid, value.group(1) if value else "", "index.html"))
    for script in sorted(web.glob("*.js")):
        if script.name.startswith("i18n") or script.name.endswith(".min.js"):
            continue
        for m in CALL.finditer(script.read_text(encoding="utf-8")):
            uses.append((m.group(2), m.group(3) if m.group(3) is not None else m.group(4), script.name))
    return uses


def edges(text):
    # Leading/trailing whitespace and the final sentence mark must survive translation.
    lead = text[:len(text) - len(text.lstrip())]
    trail = text[len(text.rstrip()):]
    body = text.strip()
    return lead, trail, body[-1:] if body[-1:] in tuple(".:?!…") else ""


def check(app):
    web = app / "assets" / "web"
    errors, table, seen = [], {}, set()
    for m in ENTRY.finditer((web / "i18n-it.js").read_text(encoding="utf-8")):
        tid = m.group(1)
        if tid in seen:
            errors.append(f"duplicate id in i18n-it.js: {tid}")
        seen.add(tid)
        table[tid] = m.group(2) if m.group(2) is not None else m.group(3)
    english_of = {}
    for tid, english, name in collect_uses(web):
        if tid in english_of and english_of[tid][0] != english:
            errors.append(f"conflicting english for {tid}: {english_of[tid][1]} vs {name}")
        english_of.setdefault(tid, (english, name))
        if tid not in table:
            errors.append(f"missing in it: {tid}")
        elif HOLDER.findall(english) != HOLDER.findall(table[tid]):
            errors.append(f"placeholder mismatch: {tid}")
        elif edges(english) != edges(table[tid]):
            errors.append(f"edge mismatch: {tid}")
    for tid in sorted(table):
        if tid not in english_of:
            errors.append(f"unused table id: {tid}")
    return errors


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("app", nargs="?", default=str(Path(__file__).resolve().parents[1]))
    args = parser.parse_args(argv)
    errors = check(Path(args.app))
    for line in errors:
        print(line)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

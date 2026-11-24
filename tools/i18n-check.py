#!/usr/bin/env python3
"""Release check for the string catalog (not a unit test).

Usage: i18n-check.py [app_dir]

Collects every TR("id", "english") from <app>/src/*.cpp and every
{"id", "english", "italiano"} entry from src/i18n_it.cpp. Exit 1 when a used
ID has no Italian entry, a table ID is unused, two uses of one ID disagree on
the English text, an English/Italian pair has different placeholders, or the
table repeats an ID. English and Italian must also agree on leading and
trailing whitespace and on the number of newlines.
"""
import argparse
import re
import sys
from pathlib import Path

LIT = r'"(?:[^"\\]|\\.)*"'
USE = re.compile(r'\bTR\(\s*"([a-z0-9_.]+)"\s*,\s*(' + LIT + r'(?:\s*' + LIT + r')*)\s*\)')
MULTI = r'((?:' + LIT + r'\s*)+)'
ENTRY = re.compile(r'\{\s*"([a-z0-9_.]+)"\s*,\s*' + MULTI + r',\s*' + MULTI + r'\}')
HOLDER = re.compile(
    r'%(?:%|[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcsp])')


def holders(text):
    return HOLDER.findall(text)


def edges(text):
    """Leading and trailing whitespace (with \\n unescaped) and the newline count."""
    plain = text.replace("\\n", "\n")
    return (plain[:len(plain) - len(plain.lstrip())], plain[len(plain.rstrip()):],
            plain.count("\n"))


def join(literals):
    return "".join(p[1:-1] for p in re.findall(LIT, literals))


def read_table(path):
    return [(m.group(1), join(m.group(2)), join(m.group(3)))
            for m in ENTRY.finditer(Path(path).read_text(encoding="utf-8"))]


def collect_uses(app):
    uses = []
    for path in sorted((app / "src").glob("*.cpp")):
        if path.name == "i18n_it.cpp":
            continue
        for m in USE.finditer(path.read_text(encoding="utf-8")):
            english = join(m.group(2))
            uses.append((m.group(1), english, path.name))
    return uses


def check(app):
    errors = []
    table = (app / "src" / "i18n_it.cpp").as_posix()
    table_ids = set()
    for tid, english, italiano in read_table(table):
        if tid in table_ids:
            errors.append(f"duplicate id in {Path(table).name}: {tid}")
        table_ids.add(tid)
        if holders(english) != holders(italiano):
            errors.append(f"placeholder mismatch: {tid}")
        if edges(english) != edges(italiano):
            errors.append(f"whitespace mismatch: {tid}")
    english_of = {}
    for tid, english, name in collect_uses(app):
        if tid in english_of and english_of[tid][0] != english:
            errors.append(f"conflicting english for {tid}: {english_of[tid][1]} vs {name}")
        english_of.setdefault(tid, (english, name))
        if tid not in table_ids:
            errors.append(f"missing in it: {tid}")
    for tid in sorted(table_ids):
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

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Helpers for idempotent source preparation during incremental builds."""

from pathlib import Path


def write_text_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text() == content:
        return
    path.write_text(content)

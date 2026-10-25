#!/usr/bin/env python3
"""Turns the interesting lines of a failed CI step into a GitHub error annotation.

Annotations are visible in the run summary and readable through the checks API, which makes
platform-specific failures (macOS, Windows) debuggable without downloading logs.

    some-command 2>&1 | tee out.log || python3 tools/ci_annotate.py "title" out.log
"""
import re
import sys

title, path = sys.argv[1], sys.argv[2]
lines = open(path, errors="replace").read().splitlines()
pattern = re.compile(r"error|fail|\*\*\*|assert|check|expected|actual", re.I)
picked = [l for l in lines if pattern.search(l)][:25] or lines[-25:]
text = "\n".join(picked)[:3500]
text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
print(f"::error title={title}::{text}")
sys.exit(1)

#!/usr/bin/env python3
"""Compatibility entrypoint; the canonical server lives in scripts/show.py."""

import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("edgeflow_show", ROOT / "scripts" / "show.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


if __name__ == "__main__":
    MODULE.launch_web(None, int(sys.argv[1]) if len(sys.argv) > 1 else 8080)

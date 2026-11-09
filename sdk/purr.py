#!/usr/bin/env python3
"""purr: the PURR Developer Kit's one command. `purr` with no arguments opens PURR Studio."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from purrsdk.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

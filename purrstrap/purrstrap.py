#!/usr/bin/env python3
"""purrstrap: PURR OS build and packaging tool. See SPEC.md."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib.app import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())

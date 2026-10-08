#!/usr/bin/env python3
"""Launcher shared-graph-memory.mdbx (общая память роя на libmdbx)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mcp.mcp_server import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
"""shared-graph-memory.mdbx — персистентная память роя на libmdbx (Python + cffi).

Начальная точка документации: skynet/MCP-shared-graph-memory/docs/DESIGN.md.
"""

from . import errors, index, libmdbx, normalize
from .store import Store

__all__ = ["Store", "errors", "index", "libmdbx", "normalize"]
__version__ = "0.1.0"
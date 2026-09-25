"""mcp-memory — персистентная память роя на libmdbx (Python + cffi).

Начальная точка документации: skynet/mcp-memory-design.md.
"""

from . import errors, index, libmdbx, normalize
from .store import Store

__all__ = ["Store", "errors", "index", "libmdbx", "normalize"]
__version__ = "0.1.0"
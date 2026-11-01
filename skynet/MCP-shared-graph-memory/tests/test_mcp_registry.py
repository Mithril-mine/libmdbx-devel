"""Целостность реестра инструментов: TOOLS-схема ↔ _dispatch (A7).

Дрейф «схема есть, а ветки dispatch нет» (и наоборот) уже дважды приводил к
багам (trace_path limit, probe_status required). Тест гарантирует:
1) статически — каждое имя из tools/list имеет ветку в _dispatch и каждая
   ветка _dispatch зарегистрирована в tools;
2) рантаймом — каждый инструмент диспетчеризуется без NO_SUCH_TOOL и без
   KeyError на обязательных аргументах (по схеме inputSchema.required).
"""

import ast
import os
from unittest.mock import MagicMock

import pytest

from mcp.mcp_server import McpServer

SERVER_SRC = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), "mcp", "mcp_server.py")


@pytest.fixture()
def server():
    return McpServer(MagicMock())


def _dispatch_branch_names() -> set:
    """Имена из веток `if name == "..."` внутри _dispatch (AST-парсинг)."""
    with open(os.path.normpath(SERVER_SRC), encoding="utf-8") as fh:
        src = fh.read()
    tree = ast.parse(src)
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == "_dispatch":
            for sub in ast.walk(node):
                if (isinstance(sub, ast.Compare)
                        and isinstance(sub.left, ast.Name)
                        and sub.left.id == "name"
                        and len(sub.ops) == 1
                        and isinstance(sub.ops[0], ast.Eq)
                        and isinstance(sub.comparators[0], ast.Constant)
                        and isinstance(sub.comparators[0].value, str)):
                    names.add(sub.comparators[0].value)
    return names


def _dummy_for(schema_prop: dict):
    t = schema_prop.get("type")
    if t == "string":
        return "x"
    if t == "integer":
        return 1
    if t == "number":
        return 1.0
    if t == "array":
        return []
    if t == "object":
        return {}
    if t == "boolean":
        return True
    return "x"


def _minimal_args(tool: dict) -> dict:
    schema = tool.get("inputSchema") or {}
    args = {}
    for req in schema.get("required") or []:
        prop = (schema.get("properties") or {}).get(req, {})
        args[req] = _dummy_for(prop)
    # отдельные инструменты требуют алиасы/специфику сверх required
    name = tool["name"]
    if name == "cg_edges_of":
        args["caller"] = "fn:x"
    if name == "cg_callers_of":
        args["callee"] = "fn:x"
    if name == "db_backup":
        args["skip"] = True  # простой путь без PROMPT-ветки
    return args


def test_registry_dispatch_parity(server):
    """tools/list и ветки _dispatch совпадают 1-к-1 (нет дрейфа)."""
    registered = set(server.tools.keys())
    branches = _dispatch_branch_names()
    assert registered - branches == set(), \
        "инструменты без ветки dispatch: %s" % sorted(registered - branches)
    assert branches - registered == set(), \
        "ветки dispatch без регистрации: %s" % sorted(branches - registered)


def test_every_tool_dispatches(server):
    """Каждый инструмент диспетчеризуется (нет NO_SUCH_TOOL/KeyError)."""
    from mcp.errors import MemoryError as ME
    failed = []
    for name, tool in sorted(server.tools.items()):
        args = _minimal_args(tool)
        try:
            server._dispatch(name, args)
        except ME as e:
            if "NO_SUCH_TOOL" in str(e):
                failed.append((name, "NO_SUCH_TOOL"))
            # прочие контрактные ошибки от мок-стора допустимы
        except KeyError as e:
            failed.append((name, "KeyError %s" % e))
        except Exception:  # noqa: BLE001
            # MagicMock-арифметика и т.п. — не дрейф реестра; нам важны
            # только NO_SUCH_TOOL/KeyError
            pass
    assert not failed, "проблемы диспетчеризации: %s" % failed


def test_registry_schemas_wellformed(server):
    """Каждая схема — object с properties; required ⊆ properties."""
    for name, tool in server.tools.items():
        schema = tool.get("inputSchema")
        assert isinstance(schema, dict), name
        assert schema.get("type") == "object", name
        props = schema.get("properties") or {}
        for req in schema.get("required") or []:
            assert req in props, "%s: required %r вне properties" % (name, req)
        assert tool.get("description"), "%s: пустое описание" % name

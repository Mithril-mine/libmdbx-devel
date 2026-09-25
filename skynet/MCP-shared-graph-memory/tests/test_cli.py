"""Тесты memory-cli (подпроцессы)."""

import json
import os
import subprocess
import sys

import pytest

from tests.conftest import seed_vocab

CLI = [sys.executable, "-m", "mcp_memory.cli"]


@pytest.fixture
def seeded(store_path):
    from mcp_memory import Store
    s = Store(store_path)
    seed_vocab(s, [("crypto", "alignment"), ("platform", "android"),
                   ("platform", "android-abi")])
    s.safe_store("bug:crypto:alignment-arm64", "bug",
                 "Segfault misaligned NEON.", 0.7)
    s.safe_store("fact:platform:android-abi", "fact", "Android ABIs.", 0.9)
    s.close()
    return store_path


def run(args):
    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return subprocess.run(CLI + args, capture_output=True, text=True, env=env)


def test_cli_stats(seeded):
    r = run(["--path", seeded, "stats"])
    assert r.returncode == 0
    st = json.loads(r.stdout)
    assert st["records"] == 2


def test_cli_dump_type_filter(seeded):
    r = run(["--path", seeded, "dump", "--type", "bug"])
    assert r.returncode == 0
    assert "bug:crypto:alignment" in r.stdout


def test_cli_dump_key(seeded):
    r = run(["--path", seeded, "dump", "--key", "fact:platform:android-abi"])
    assert r.returncode == 0
    body = json.loads(r.stdout)
    assert body["type"] == "fact"


def test_cli_graph_json(seeded):
    run(["--path", seeded, "purge"] if False else ["--path", seeded, "dump"])
    # нужна связь — создадим через Store напрямую
    from mcp_memory import Store
    s = Store(seeded)
    s.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    s.close()
    r = run(["--path", seeded, "graph", "--key", "bug:crypto:alignment"])
    assert r.returncode == 0
    g = json.loads(r.stdout)
    assert len(g["edges"]) == 1


def test_cli_graph_dot(seeded):
    from mcp_memory import Store
    s = Store(seeded)
    s.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    s.close()
    r = run(["--path", seeded, "graph", "--key", "bug:crypto:alignment",
             "--format", "dot"])
    assert r.returncode == 0
    assert r.stdout.startswith("digraph memory")


def test_cli_gc(seeded):
    r = run(["--path", seeded, "gc"])
    assert r.returncode == 0
    res = json.loads(r.stdout)
    assert res["dry_run"] is True


def test_cli_vocab(seeded):
    r = run(["--path", seeded, "vocab"])
    assert r.returncode == 0
    lst = json.loads(r.stdout)
    assert "crypto" in lst["modules"]


def test_cli_purge(seeded):
    r = run(["--path", seeded, "purge", "bug:crypto:alignment"])
    assert r.returncode == 0
    res = json.loads(r.stdout)
    assert res["removed"] == ["bug:crypto:alignment"]


def test_cli_no_args_help(seeded):
    r = run(["--path", seeded])
    assert r.returncode == 1
"""Прямые вызовы cli.main (для покрытия кода cli.py)."""

import json

import pytest

from mcp_memory.cli import main
from tests.conftest import seed_vocab


@pytest.fixture
def seeded(store_path):
    from mcp_memory import Store
    s = Store(store_path)
    seed_vocab(s, [("crypto", "alignment"), ("platform", "android"),
                   ("platform", "android-abi")])
    s.safe_store("bug:crypto:alignment-arm64", "bug",
                 "Segfault misaligned NEON.", 0.7)
    s.safe_store("fact:platform:android-abi", "fact", "Android ABIs.", 0.9)
    s.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    s.close()
    return store_path


def test_main_stats(seeded, capsys):
    assert main(["--path", seeded, "stats"]) == 0
    st = json.loads(capsys.readouterr().out)
    assert st["records"] == 2


def test_main_dump_all(seeded, capsys):
    assert main(["--path", seeded, "dump"]) == 0
    out = capsys.readouterr().out
    assert "bug:crypto:alignment" in out


def test_main_dump_module_filter(seeded, capsys):
    assert main(["--path", seeded, "dump", "--module", "crypto"]) == 0
    out = capsys.readouterr().out
    assert "bug:crypto:alignment" in out
    assert "fact:platform" not in out


def test_main_dump_missing_key(seeded, capsys):
    assert main(["--path", seeded, "dump", "--key", "fact:platform:android"]) == 0
    assert capsys.readouterr().out.strip() == "NOTFOUND"


def test_main_graph_dot(seeded, capsys):
    assert main(["--path", seeded, "graph", "--key", "bug:crypto:alignment",
                 "--format", "dot"]) == 0
    assert capsys.readouterr().out.startswith("digraph memory")


def test_main_audit(seeded, capsys):
    assert main(["--path", seeded, "audit"]) == 0


def test_main_gc_execute_archive(seeded, capsys):
    assert main(["--path", seeded, "gc", "--execute", "--archive"]) == 0
    res = json.loads(capsys.readouterr().out)
    assert res["dry_run"] is False


def test_main_vocab_add_module(seeded, capsys):
    assert main(["--path", seeded, "vocab", "--add", "module", "--value", "zzmod"]) == 0
    assert "added" in capsys.readouterr().out


def test_main_vocab_add_topic(seeded, capsys):
    assert main(["--path", seeded, "vocab", "--add", "topic",
                 "--value", "crypto:zz-topic"]) == 0


def test_main_vocab_find(seeded, capsys):
    assert main(["--path", seeded, "vocab", "--find", "crypt"]) == 0
    out = capsys.readouterr().out
    assert "crypto" in out


def test_main_purge(seeded, capsys):
    assert main(["--path", seeded, "purge", "bug:crypto:alignment"]) == 0
    res = json.loads(capsys.readouterr().out)
    assert res["removed"] == ["bug:crypto:alignment"]


def test_main_no_args(seeded, capsys):
    assert main(["--path", seeded]) == 1


def test_main_dump_type_filter(seeded, capsys):
    assert main(["--path", seeded, "dump", "--type", "bug"]) == 0
    out = capsys.readouterr().out
    assert "bug:crypto:alignment" in out
    assert "fact:platform" not in out


def test_main_graph_json(seeded, capsys):
    assert main(["--path", seeded, "graph", "--key", "bug:crypto:alignment",
                 "--format", "json"]) == 0
    g = json.loads(capsys.readouterr().out)
    assert len(g["edges"]) == 1


def test_main_vocab_list(seeded, capsys):
    assert main(["--path", seeded, "vocab"]) == 0
    lst = json.loads(capsys.readouterr().out)
    assert "crypto" in lst["modules"]


def test_main_default_path_env(seeded, capsys, monkeypatch):
    monkeypatch.setenv("MEMORY_MDBX_PATH", seeded)
    assert main(["stats"]) == 0
    st = json.loads(capsys.readouterr().out)
    assert st["records"] == 2
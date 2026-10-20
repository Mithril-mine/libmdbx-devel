"""Тесты слоя «карта проекта» (project_files / file_deps)."""


def test_project_load_and_files(store):
    store.project_load({
        "CMakeLists.txt": {"kind": "cmake", "lang": "cmake", "module": "_root"},
        "docs/en/overview/restrictions.md": {"kind": "doc", "lang": "md",
                                             "module": "docs"},
        "src/cursor.c": {"kind": "code", "lang": "c", "module": "src"},
    }, [
        {"from": "CMakeLists.txt", "to": "cmake/compiler.cmake", "kind": "file"},
        {"from": "docs/en/overview/restrictions.md", "to": "CMakeLists.txt",
         "kind": "file"},
    ])
    files = store.project_files("docs/")
    assert [f["path"] for f in files] == ["docs/en/overview/restrictions.md"]
    assert files[0]["meta"]["kind"] == "doc"
    deps = store.file_deps("CMakeLists.txt")
    assert "cmake/compiler.cmake" in deps
    dependents = store.file_dependents("CMakeLists.txt")
    assert "docs/en/overview/restrictions.md" in dependents
    # replace очищает старые данные
    store.project_load({"x": {"kind": "misc", "lang": "txt", "module": "_root"}},
                       [], replace=True)
    assert len(store.project_files()) == 1
    assert store.file_deps("CMakeLists.txt") == []


def test_project_load_additive(store):
    store.project_load({"a.txt": {"kind": "misc", "lang": "txt", "module": "m"}},
                       [])
    store.project_load({"b.txt": {"kind": "misc", "lang": "txt", "module": "m"}},
                       [], replace=False)
    paths = {f["path"] for f in store.project_files()}
    assert paths == {"a.txt", "b.txt"}


def test_scan_project_cmake_add_test_name(tmp_path):
    """add_test(NAME <n> ...) парсится в имя теста (не литерал NAME)."""
    import tools.scan_project as sp
    f = tmp_path / "CMakeLists.txt"
    f.write_text(
        "add_library(mdbx foo.c)\n"
        "add_test(NAME mcp_pytest\n"
        "  COMMAND python3 -m pytest)\n"
        "function(my_func)\nendfunction()\n")
    syms = sp.scan_symbols(str(f), str(tmp_path), {str(f): {"kind": "cmake",
                                                            "lang": "cmake"}})
    keys = set(syms.keys())
    assert any("mcp_pytest" in k for k in keys), keys
    assert any("#mdbx" in k for k in keys)
    assert any("my_func" in k for k in keys)


def test_run_coverage_helpers(tmp_path):
    """run_coverage: clear_gcda удаляет .gcda; gcda_dirs находит каталоги."""
    import sys
    import os
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "tools"))
    import run_coverage as rc
    bd = tmp_path / "build"
    (bd / "a" / "b").mkdir(parents=True)
    (bd / "a" / "b" / "x.gcda").write_text("d")
    (bd / "a" / "y.gcda").write_text("d")
    (bd / "a" / "y.gcno").write_text("n")  # не gcda — остаётся
    dirs = rc.gcda_dirs(str(bd))
    assert len(dirs) == 2
    n = rc.clear_gcda(str(bd))
    assert n == 2
    assert rc.gcda_dirs(str(bd)) == []
    assert (bd / "a" / "y.gcno").exists()  # gcno не тронут

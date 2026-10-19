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
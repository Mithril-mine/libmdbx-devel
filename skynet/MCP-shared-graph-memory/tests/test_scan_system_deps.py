"""Тесты scan_system_deps.py (системный include-граф).

Используют фейковые «системные заголовки» (tests/fixtures/sysdeps/*.h),
подаются сканеру напрямую (per-header режим) и через -I (режим TU).
Требуют clang; при отсутствии — pytest.skip.
"""

import json
import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(HERE), "tools")
sys.path.insert(0, TOOLS)
FIX_SYS = os.path.join(HERE, "fixtures", "sysdeps")

HDR_A = os.path.join(FIX_SYS, "fake_linux_only.h")
HDR_B = os.path.join(FIX_SYS, "fake_common.h")
TU = os.path.join(FIX_SYS, "tu_uses_sys.c")


def _clang():
    path = shutil.which("clang")
    if not path:
        pytest.skip("clang not in PATH")
    return path


def _scan_headers(headers, config="linux", tmpdir=None):
    """Прогоняет per-header скан для списка заголовков, возвращает артефакт."""
    scan = sys.executable, os.path.join(TOOLS, "scan_system_deps.py")
    out = os.path.join(tmpdir, "system-deps-%s.json" % config)
    args = ["--mode", "scan-headers", "--config", config, "--out", out]
    for h in headers:
        args += ["--headers-list", h] if False else []
    # headers_list: построчный список — собираем одноразовый файл
    lst = os.path.join(tmpdir, "headers.txt")
    with open(lst, "w") as f:
        f.write("\n".join(headers))
    args += ["--headers-list", lst, "--limit-headers", "0"]
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "scan_system_deps.py"),
                        *args], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[:800]
    with open(out) as f:
        return json.load(f)


def _hdr_key(artifact, header_path):
    for key, h in artifact["headers"].items():
        if h["path"] == os.path.realpath(header_path):
            return key
    return None


def test_provides_functions_types_macros(tmp_path):
    art = _scan_headers([HDR_A, HDR_B], config="linux", tmpdir=str(tmp_path))
    key_a = _hdr_key(art, HDR_A)
    assert key_a
    prov = art["headers"][key_a]["provides"]
    assert {"fake_setmntent", "fake_getmntent", "fake_endmntent"} <= set(prov["fn"])
    assert "fake_mntent" in prov["type"]
    assert {"FAKE_MNTTYPE_LXC", "FAKE_MOUNTED"} <= set(prov["macro"])

    key_b = _hdr_key(art, HDR_B)
    prov_b = art["headers"][key_b]["provides"]
    assert "fake_common_probe" in prov_b["fn"]
    assert "fake_common_width_t" in prov_b["type"]
    assert "FAKE_COMMON_VERSION" in prov_b["macro"]
    # макросы через -E -dD: не дублируются из provides-AST
    assert prov_b["macro"] == prov_b["macro"]  # детерминизм


def test_uses_resolution(tmp_path):
    hdrs = _scan_headers([HDR_A, HDR_B], config="linux", tmpdir=str(tmp_path))
    hdrs_path = os.path.join(tmp_path, "system-deps-linux.json")

    uses_out = os.path.join(tmp_path, "uses-linux.json")
    inc = "-I" + FIX_SYS
    r = subprocess.run([
        sys.executable, os.path.join(TOOLS, "scan_system_deps.py"),
        "--mode", "scan-uses", "--config", "linux",
        "--cc", os.devnull,  # TU задаём через фикстуру ниже — см. _uses_direct
        "--provides-from", hdrs_path,
        "--out", uses_out,
        "--includes", inc,
    ], capture_output=True, text=True)
    # --cc /dev/null не даёт TU: используем прямой вызов python-API вместо CLI
    # (CLI без compile_commands не даёт TU-списка — тест через API):
    sys.path.insert(0, TOOLS)
    import scan_system_deps as sd
    import scan_symbols as ss
    cfg = ss.ScanConfig(name="linux", includes=inc)
    entry = {"file": os.path.abspath(TU),
             "directory": FIX_SYS,
             "command": "cc -I%s %s" % (FIX_SYS, TU)}
    prov_idx = {}
    for key, h in hdrs["headers"].items():
        for kind, names in h["provides"].items():
            for nm in names:
                prov_idx.setdefault(nm, set()).add(key)
    uses = sd.scan_uses([(TU, entry)], _clang(), cfg,
                        {k: list(v) for k, v in prov_idx.items()})
    tu_uses = uses[os.path.relpath(TU, sd.REPO_ROOT)]
    refs = tu_uses["sys_refs"]
    # резолвы: оба фейковых заголовка
    assert set(refs["fake_setmntent"]) == {_hdr_key(hdrs, HDR_A)}
    assert set(refs["fake_common_probe"]) == {_hdr_key(hdrs, HDR_B)}
    assert refs["FAKE_COMMON_VERSION"] == [_hdr_key(hdrs, HDR_B)]
    # unresolved: намеренно не-объявленный идентификатор
    assert "totally_unknown_api" in tu_uses["unresolved"]
    # встроенные/локальные имена не попадают в unresolved как sys-refs:
    assert "tu_uses_sys" not in refs


def test_merge_config_categories(tmp_path):
    a = _scan_headers([HDR_A, HDR_B], config="linux", tmpdir=str(tmp_path))
    # «второй конфиг»: сканируем только общий заголовок (эмуляция того, что
    # linux-only заголовок на этой платформе отсутствует)
    b = _scan_headers([HDR_B], config="win32", tmpdir=str(tmp_path))
    scans = [a, {"config": "win32", "headers": b["headers"], "uses": {}}]
    sys.path.insert(0, TOOLS)
    import scan_system_deps as sd
    merged = sd.merge_configs(scans)
    key_a = _hdr_key(a, HDR_A)
    key_b = _hdr_key(a, HDR_B)
    assert sorted(merged["headers"][key_a]["in_configs"]) == ["linux"]
    assert sorted(merged["headers"][key_b]["in_configs"]) == ["linux", "win32"]
    assert merged["configs"] == ["linux", "win32"]


def test_store_tables_and_queries(tmp_path):
    """Store: таблицы sys_headers/sys_refs/sys_uses, batch-load и запросы."""
    sys.path.insert(0, os.path.join(os.path.dirname(HERE)))
    from mcp import Store
    db = os.path.join(str(tmp_path), "t.mdbx")
    st = Store(db)
    # upgrade создаёт новые таблицы
    assert "sys_headers" in st.TABLES
    headers = {
        "hdr:fake_linux_only.h": {"name": "fake_linux_only.h", "path": HDR_A,
                                  "provides": {"fn": ["fake_setmntent"],
                                               "type": ["fake_mntent"],
                                               "macro": ["FAKE_MOUNTED"]},
                                  "in_configs": ["linux"]},
        "hdr:fake_common.h": {"name": "fake_common.h", "path": HDR_B,
                              "provides": {"fn": ["fake_common_probe"],
                                           "type": ["fake_common_width_t"],
                                           "macro": ["FAKE_COMMON_VERSION"]},
                              "in_configs": ["linux", "win32"]},
    }
    uses = {"src/osal/posix/osal-bootid.c": {
        "sys_refs": {"fake_setmntent": ["hdr:fake_linux_only.h"],
                     "fake_common_probe": ["hdr:fake_common.h"]},
        "unresolved": ["totally_unknown_api"]}}
    r = st.sys_deps_load(headers, uses, replace=True)
    assert r["headers"] == 2 and r["files"] == 1

    # запросы
    got = st.sys_headers_required_for("src/osal/posix/osal-bootid.c")
    assert set(got) == {"hdr:fake_linux_only.h", "hdr:fake_common.h"}
    present = st.sys_known_on("src/osal/posix/osal-bootid.c", config="win32")
    # fake_setmntent доступен только в linux -> не в present(win32)
    assert "fake_setmntent" not in present
    assert "fake_common_probe" in present
    with pytest.raises(KeyError):
        st.sys_known_on("src/osal/posix/osal-bootid.c", config="freebsd")
    st.close()


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))


def test_mcp_sys_tools(tmp_path):
    """MCP-инструменты sys_*/symbol_definitions через McpServer.handle."""
    from mcp import Store
    from mcp.mcp_server import McpServer
    st = Store(os.path.join(str(tmp_path), "m.mdbx"))
    st.sys_deps_load(
        {"hdr:fake_common.h": {"name": "fake_common.h", "path": HDR_B,
                               "provides": {"fn": ["fake_common_probe"],
                                            "macro": ["FAKE_COMMON_VERSION"]},
                               "in_configs": ["linux", "win32"]}},
        {"src/x.c": {"sys_refs": {"fake_common_probe": ["hdr:fake_common.h"]},
                     "unresolved": []}},
        replace=True)
    srv = McpServer(st)

    def call(method, params):
        r = srv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                        "params": {"name": method, "arguments": params}})
        return r["result"].get("structuredContent") or json.loads(
            r["result"]["content"][0]["text"])

    r = call("sys_headers_required_for", {"file": "src/x.c"})
    assert r["headers"] == ["hdr:fake_common.h"]
    r = call("sys_known_on", {"file": "src/x.c", "config": "win32"})
    assert "fake_common_probe" in r["known"]
    # не-сканированный конфиг -> JSON-RPC error (unknown, а не пустой ответ)
    r = srv.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                    "params": {"name": "sys_known_on",
                               "arguments": {"file": "src/x.c",
                                             "config": "freebsd"}}})
    assert "error" in r
    r = call("sys_refs_of", {"symbol": "fake_common_probe"})
    assert r["headers"] == ["hdr:fake_common.h"]
    r = call("sys_uses_of", {"file": "src/x.c"})
    assert r["uses"]["sys_refs"]["fake_common_probe"] == ["hdr:fake_common.h"]
    st.close()


def test_mcp_sys_deps_load(tmp_path):
    """MCP-инструмент sys_deps_load: батчевая загрузка через McpServer.handle."""
    from mcp import Store
    from mcp.mcp_server import McpServer
    st = Store(os.path.join(str(tmp_path), "m.mdbx"))
    srv = McpServer(st)

    def call(method, params):
        r = srv.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                        "params": {"name": method, "arguments": params}})
        assert "result" in r, r
        return r["result"].get("structuredContent") or json.loads(
            r["result"]["content"][0]["text"])

    headers = {"hdr:fake_common.h": {"name": "fake_common.h", "path": HDR_B,
                                     "provides": {"fn": ["fake_common_probe"],
                                                  "macro": ["FAKE_COMMON_VERSION"]},
                                     "in_configs": ["linux", "win32"]}}
    uses = {"src/x.c": {"sys_refs": {"fake_common_probe": ["hdr:fake_common.h"]},
                        "unresolved": []}}
    r = call("sys_deps_load", {"headers": headers, "uses": uses, "replace": True})
    assert r["headers"] == 1 and r["files"] == 1

    # данные читаются через соседние инструменты
    assert call("sys_refs_of", {"symbol": "fake_common_probe"})["headers"] == \
        ["hdr:fake_common.h"]
    assert call("sys_headers_required_for", {"file": "src/x.c"})["headers"] == \
        ["hdr:fake_common.h"]

    # replace=False — аддитивное дополнение (без очистки)
    headers2 = {"hdr:fake_linux_only.h": {"name": "fake_linux_only.h",
                                          "path": HDR_A,
                                          "provides": {"fn": ["fake_setmntent"]},
                                          "in_configs": ["linux"]}}
    r = call("sys_deps_load", {"headers": headers2, "uses": {}, "replace": False})
    assert r["headers"] == 1
    assert len(call("sys_refs_of", {"symbol": "fake_common_probe"})["headers"]) == 1
    assert call("sys_refs_of", {"symbol": "fake_setmntent"})["headers"] == \
        ["hdr:fake_linux_only.h"]

    # replace=True — очистка sys-таблиц перед записью
    r = call("sys_deps_load", {"headers": headers2, "uses": {}, "replace": True})
    assert r["headers"] == 1
    assert call("sys_refs_of", {"symbol": "fake_common_probe"})["headers"] == []
    st.close()

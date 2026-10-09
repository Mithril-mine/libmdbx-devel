#!/usr/bin/env python3
# Infra v3 single-cell runner (Python). Replaces the former bash+cygpath
# wrapper: native path handling on Windows and POSIX alike, no shell quoting /
# escape pitfalls, same logic locally and in CI (codex C5).
#
# Usage:
#   tests/ci/run-cell.py <cell-id> [--scope fast|full] [--build-dir <dir>]
#
# Parses tests/ci/config.json, configures+builds+tests exactly one "cell"
# (a build configuration mirrored from the legacy workflow matrices). Pure
# CMake/CTest, no ci.sh. Mirrors the legacy invocation: cmake is run from the
# build dir with a RELATIVE source path (".."), never -S/-B with absolute
# paths (that combination breaks MSVC/MinGW toolchain detection on GitHub
# hosted Windows runners).
#
# Exit codes:
#   0  success (or build_only cell built OK)
#   1  build/test failure
#   2  unknown cell id / usage error
#
# Env knobs:
#   RUN_CELL_EXTRA_CMAKE  extra CMake args for the configure step (policy
#                         flags like -DINTERPROCEDURAL_OPTIMIZATION=OFF)
#   CI_CTEST_TIMEOUT      ctest per-test timeout override (default 600)

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
REGISTRY = REPO_ROOT / "tests" / "ci" / "config.json"


def expand_vars(text: str, env: dict) -> str:
    """Expand ${NAME} placeholders against the current environment."""

    def repl(match):
        return env.get(match.group(1), "")

    return re.sub(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", repl, text)


def parse_registry(cell_id: str) -> dict:
    if not REGISTRY.is_file():
        print(f"error: registry not found: {REGISTRY}", file=sys.stderr)
        sys.exit(2)
    cfg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    for cell in cfg["cells"]:
        if cell["id"] == cell_id:
            return cell
    print(f"error: unknown cell id '{cell_id}' (see tests/ci/config.json)", file=sys.stderr)
    sys.exit(2)


def normalize_path_component(component: str) -> str:
    """Convert a POSIX-style path like /c/mingw64/bin to Windows form when
    running on native Windows. The registry stores the legacy git-bash PATH
    entries verbatim; without this, native Windows tools cannot see them."""
    if os.name == "nt":
        match = re.match(r"^/([a-zA-Z])(/.*)?$", component)
        if match:
            return f"{match.group(1).upper()}:{match.group(2) or '/'}"
    return component


def join_path_list(value: str) -> str:
    """Registry PATH values use POSIX ':' separators regardless of platform;
    re-join with the native separator after normalizing each component."""
    components = [normalize_path_component(p) for p in value.split(":") if p]
    return os.pathsep.join(components)


def run_emulator_tests(cell, build_dir, scope) -> int:
    """Run the cross-compiled Android test binaries on a booted emulator (B5).

    Expected to be invoked from CI under reactivecircus/android-emulator-runner
    (the device is already booted); pushes the binaries into /data/local/tmp/mdbx
    and replays the quick-smoke scenarios through `adb shell`, recording
    per-test metrics the same way the ctest path does.
    """
    import shutil
    import time

    if not shutil.which("adb"):
        print("error: 'adb' not found in PATH (Android platform-tools required)",
              file=sys.stderr)
        return 2

    def locate(name):
        for sub in ("", "Release/", "Debug/"):
            path = Path(build_dir) / (sub + name)
            if path.is_file():
                return path
        return None

    mdbx_test = locate("mdbx_test")
    mdbx_chk = locate("mdbx_chk")
    if mdbx_test is None:
        print("error: mdbx_test binary not found in the build dir", file=sys.stderr)
        return 1

    def adb(args):
        return run(["adb", *args])

    started_ns = time.time_ns()
    # The CI emulator action already waited for boot; this is a cheap guard
    # against a wedged adb transport, not a substitute for boot completion.
    for _ in range(30):
        probe = run(["adb", "get-state"], capture=True)
        if probe.returncode == 0 and probe.stdout.strip() == "device":
            break
        time.sleep(2)
    else:
        print("error: no Android device online (adb get-state timed out)",
              file=sys.stderr)
        return 1

    remote_dir = "/data/local/tmp/mdbx"
    if adb(["shell", f"mkdir -p {remote_dir}"]) != 0:
        print("error: adb shell mkdir failed", file=sys.stderr)
        return 1
    binaries = [str(mdbx_test)]
    if mdbx_chk:
        binaries.append(str(mdbx_chk))
    if adb(["push", *binaries, remote_dir]) != 0:
        print("error: adb push failed", file=sys.stderr)
        return 1
    chmod_cmd = f"chmod 755 {remote_dir}/mdbx_test"
    if mdbx_chk:
        chmod_cmd += f" {remote_dir}/mdbx_chk"
    if adb(["shell", chmod_cmd]) != 0:
        print("error: adb chmod failed", file=sys.stderr)
        return 1

    # Quick-smoke replay (mirrors tests/CMakeLists.txt smoke_basic/smoke_chk,
    # tuned down for the emulator: shorter duration, repeat=2, bounded timeout).
    smoke = [
        "./mdbx_test", "--duration=60", "--table=+data.integer", "--keygen.split=29",
        "--datalen.min=min", "--datalen.max=max", "--progress", "--console=no",
        "--mode=+nosync-safe", "--repeat=2", "--timeout=300",
        "--pathname=smoke.db", "--dont-cleanup-after", "basic",
    ]
    scenarios = [("smoke_basic", smoke)]
    if mdbx_chk:
        scenarios.append(("smoke_chk", ["./mdbx_chk", "-vvn", "smoke.db"]))

    per_test = []
    rc_final = 0
    for name, cmd in scenarios:
        start = time.time_ns()
        rc = adb(["shell", f"cd {remote_dir} && " + " ".join(cmd)])
        sec = round((time.time_ns() - start) / 1e9, 3)
        status = "Passed" if rc == 0 else "Failed"
        per_test.append({"test": name, "status": status, "sec": sec})
        print(f"==> {name}: {status} ({sec}s)")
        if rc != 0:
            rc_final = rc

    elapsed_ms = (time.time_ns() - started_ns) // 1_000_000
    write_metrics(build_dir, cell["id"], scope, rc_final, elapsed_ms, per_test)
    if rc_final != 0:
        print(f"==> cell {cell['id']} FAILED on emulator (rc={rc_final})",
              file=sys.stderr)
    return rc_final


def run(argv, cwd=None, env=None, capture=False):
    display = " ".join(str(a) for a in argv)
    if cwd is not None:
        display = f"(cd {cwd} && {display})"
    print(f"==> {display}")
    result = subprocess.run([str(a) for a in argv], cwd=cwd, env=env,
                            capture_output=capture, text=True)
    if capture:
        return result
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description="Infra v3 single-cell runner")
    parser.add_argument("cell", help="registry cell id (tests/ci/config.json)")
    parser.add_argument("--scope", choices=("fast", "full"), default="fast",
                        help="ctest scope: fast (smoke-t1|ut.* minus heavy) or full")
    parser.add_argument("--build-dir", default=None,
                        help="build directory (default @ci-cmake-build/<cell>)")
    args = parser.parse_args()

    cell = parse_registry(args.cell)
    started_ns = time_ns()

    if args.build_dir:
        build_dir = Path(args.build_dir)
        if not build_dir.is_absolute():
            build_dir = REPO_ROOT / build_dir
    else:
        build_dir = REPO_ROOT / "@ci-cmake-build" / cell["id"]
    build_dir.mkdir(parents=True, exist_ok=True)
    # Relative source path from the build dir to the repo root: this is what
    # makes native Windows cmake work (no -S/-B with absolute paths).
    source_rel = os.path.relpath(REPO_ROOT, build_dir)

    runs_on = cell.get("runs-on", "")
    build_only = bool(cell.get("build_only", False))
    print(f"==> run-cell: {cell['id']} (runs-on: {runs_on})")
    print(f"==> build dir: {build_dir}")

    # --- toolchain env ------------------------------------------------------
    env = dict(os.environ)
    for key, value in (cell.get("env") or {}).items():
        value = expand_vars(value, env)
        if key == "PATH":
            env["PATH"] = join_path_list(value) + os.pathsep + env["PATH"]
        else:
            env[key] = value

    # --- cmake args ('flag|value' pairs or plain '-D...' args) --------------
    cmake_args = []
    build_config = ""
    for arg in cell.get("cmake") or []:
        arg = expand_vars(arg, env)
        if "|" in arg:
            flag, _, value = arg.partition("|")
            cmake_args += [flag, value]
        else:
            cmake_args.append(arg)
        if arg.startswith("-DCMAKE_BUILD_TYPE="):
            build_config = arg.split("=", 1)[1]

    # Policy (TASK-23): test builds never enable LTO; ccache launchers come
    # from the environment when the image provides them.
    extra = os.environ.get("RUN_CELL_EXTRA_CMAKE", "").split()
    if extra:
        cmake_args += extra
        print(f"==> extra cmake args: {' '.join(extra)}")

    # Deterministic generator selection (legacy ci.sh semantics): default to
    # Ninja only when the cell does not pin -G/-A/-T and the build dir is new.
    generator = []
    if not (build_dir / "CMakeCache.txt").exists():
        joined = " ".join(cmake_args)
        if shutil.which("ninja") and not any(
            f" {flag} " in f" {joined} " for flag in ("-G", "-A", "-T")
        ):
            generator = ["-G", "Ninja"]

    print(f"==> cmake configure: {' '.join(cmake_args)}")
    rc = run(["cmake", *generator, *cmake_args, source_rel], cwd=build_dir, env=env)
    if rc != 0:
        return 1
    print("==> cmake build")
    # Some cells (mingw on shared Windows runners) run out of memory when the
    # whole suite compiles in parallel; pin the build parallelism explicitly.
    build_parallel = cell.get("build_parallel")
    build_parallel_args = ["--parallel", str(build_parallel)] if build_parallel else ["--parallel"]
    if build_config:
        rc = run(["cmake", "--build", ".", *build_parallel_args, "--config", build_config],
                 cwd=build_dir, env=env)
    else:
        rc = run(["cmake", "--build", ".", *build_parallel_args], cwd=build_dir, env=env)
    if rc != 0:
        return 1

    if cell.get("emulator"):
        return run_emulator_tests(cell, build_dir, args.scope)

    if build_only:
        # No CTest to run: report a zero-test cell so the metrics artifact is
        # still produced (ci-run.yml uploads it unconditionally) instead of
        # warning "No files were found".
        elapsed_ms = (time_ns() - started_ns) // 1_000_000
        write_metrics(build_dir, cell["id"], args.scope, 0, elapsed_ms, [])
        print(f"==> build_only cell: skipping ctest ({cell['id']}, wall_ms={elapsed_ms})")
        return 0

    # --- ctest --------------------------------------------------------------
    cell_ctest = cell.get("ctest") or {}
    ctest_regex = cell_ctest.get("regex") or ""
    ctest_exclude = cell_ctest.get("exclude") or ""

    if ctest_regex or ctest_exclude:
        # Cell explicitly narrows the test set (by name regex and/or exclusion);
        # honor it as-is regardless of the scope.
        by_label = False
    elif args.scope == "full":
        # Full scope: plain ctest unless the cell overrides it above.
        ctest_regex = ""
        ctest_exclude = ""
        by_label = False
    else:
        # Fast tier: select by CTest labels (smoke-t1|ut.* minus ut.heavy).
        ctest_regex = "smoke-t1|ut\\."
        ctest_exclude = "ut\\.heavy"
        by_label = True

    ctest_cmd = ["ctest", "--output-on-failure", "--parallel", "3",
                 "--schedule-random", "--no-tests=error",
                 "--timeout", os.environ.get("CI_CTEST_TIMEOUT", "600")]
    if build_config:
        ctest_cmd += ["-C", build_config]
    if by_label:
        if ctest_regex:
            ctest_cmd += ["-L", ctest_regex]
        if ctest_exclude:
            ctest_cmd += ["-LE", ctest_exclude]
    else:
        if ctest_regex:
            ctest_cmd += ["-R", ctest_regex]
        if ctest_exclude:
            ctest_cmd += ["-E", ctest_exclude]

    print(f"==> ctest: {' '.join(ctest_cmd)}")
    start = time_ns()
    result = run(ctest_cmd, cwd=build_dir, env=env, capture=True)
    elapsed_ms = (time_ns() - start) // 1_000_000
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, file=sys.stderr)

    # --- metrics (codex C4) -------------------------------------------------
    per_test = []
    pattern = re.compile(
        r"Test\s+#\d+:\s+(\S+)\s+[. ]*?(Passed|Failed|Not Run|Timeout)\s+([0-9.]+)\s+sec"
    )
    for m in pattern.finditer(result.stdout):
        per_test.append({"test": m.group(1), "status": m.group(2),
                         "sec": round(float(m.group(3)), 3)})
    write_metrics(build_dir, cell["id"], args.scope, result.returncode, elapsed_ms, per_test)

    if result.returncode != 0:
        print(f"==> cell {cell['id']} FAILED (rc={result.returncode})", file=sys.stderr)
    return result.returncode


def write_metrics(build_dir, cell_id, scope, rc, wall_ms, per_test):
    metrics = {
        "cell": cell_id,
        "scope": scope,
        "rc": rc,
        "wall_ms": wall_ms,
        "tests": per_test,
    }
    metrics_path = Path(build_dir) / "Testing" / "Temporary" / "cell-metrics.json"
    metrics_path.parent.mkdir(parents=True, exist_ok=True)
    metrics_path.write_text(json.dumps(metrics, indent=1), encoding="utf-8")
    print(f"==> cell-metrics: {metrics_path} ({len(per_test)} tests, wall_ms={wall_ms})")


def time_ns() -> int:
    import time
    return time.time_ns()


if __name__ == "__main__":
    sys.exit(main())
#!/usr/bin/env python3
"""Golden characterization tests for the src/tools CLI utilities (TASK-45).

Each case = (fixture setup) + (tool args) -> {stdout, stderr, exit-code}.
The current C-tool behavior is the CONTRACT (spec-*.md, Phase A): do NOT fix
the tools here, only record the behavior. The same suite is reused in Phase D
to validate the rewritten C++ tools 1:1.

Usage:
  golden_runner.py <tool> <tool-binary> <workdir> [--update] [<case>...]
    tool      - chk|copy|defrag|drop|dump|load|stat
    workdir   - scratch dir (created by the runner; removed on ANY exit path)
    --update  - (re)write golden files instead of comparing
    <case>... - run only the listed cases (default: all)
  golden_runner.py --selftest
    run the built-in assertions of the golden matcher (no fixtures/tools).

Golden files live next to this script under golden/<tool>/<case>.{out,err,rc}
and MUST be committed. Deterministic: fixtures are built from
fixtures/base.dump via mdbx_load (no stochastic mdbx_test runs).

## Golden line markers (the allowance mechanism)

The compared streams mix the library's own logging (e.g. the `in-core
database` NOTICE that env_open() emits when the database resides on a
memory-backed filesystem - tmpfs/ramfs/etc), which is environment noise,
not the tool's contract. Golden files may therefore carry line markers
that permit such variance, at exactly the marked positions:

  ...        a line consisting of exactly "..." is a BLOCK WILDCARD: it
             matches ZERO, one or more consecutive actual lines, i.e.
             individual lines may appear or disappear there.
  ~ TEXT     a line starting with "~ " is a PATTERN LINE: it must match
             exactly one actual line, where each "..." inside TEXT stands
             for any run of characters, i.e. the line content may vary.

Every other line must match byte-exactly (an unmarked literal "..." stays
literal - e.g. " - Taking lock... done"). Markers are written into the
golden files by hand at the positions where the variance is observed,
so each allowance is visible and reviewable. `--update` preserves the
markers via the same alignment the matcher computes (and falls back to a
plain rewrite with a warning when the output no longer matches even with
them).
"""

import argparse
import difflib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SELF_DIR = Path(__file__).resolve().parent
FIXTURES = SELF_DIR / "fixtures"
GOLDEN = SELF_DIR / "golden"

MAX_DIFF_LINES = 8  # per-stream diff cap in the FAIL diagnostics, as before

# ---------------------------------------------------------------------------
# normalize(): the volatile-field scrubber applied to BOTH sides (actual and
# golden) before the comparison. Ported 1:1 from the original bash/sed rules;
# every rule keeps the original semantics, including the anchored deletions.

NORMALIZE_SUBS = [
    (re.compile(rb"v\d+\.\d+\.\d+(-\d+)?-g[0-9a-f]+(-dirty)? \(.*\)"), rb"vVERSION (BUILD, T-HASH)"),
    (re.compile(rb"T-[0-9a-f]{40}"), rb"T-HASH"),
    (re.compile(rb"elapsed [0-9.]+ seconds"), rb"elapsed SECONDS"),
    (re.compile(rb"took [0-9.]+ seconds"), rb"took SECONDS"),
    (re.compile(rb"dxb-id [0-9a-f]+-[0-9a-f]+"), rb"dxb-id <ID>"),
    (re.compile(rb"^mdbx_[a-z_]* version [0-9.]+$"), rb"mdbx_TOOL version VERSION"),
    (re.compile(rb"^ - source: .*"), rb" - source: <SOURCE>"),
    (re.compile(rb"^ - anchor: .*"), rb" - anchor: <ANCHOR>"),
    (re.compile(rb"^ - build: .*"), rb" - build: <BUILD>"),
    (re.compile(rb"^ - flags: .*"), rb" - flags: <FLAGS>"),
    (re.compile(rb"^ - options: .*"), rb" - options: <OPTIONS>"),
    (re.compile(rb"[^ ]*/(base\.db|copy\.db|bad\.db|compact\.db|out\.img|reload\.db|ver\.db|"
                rb"bad\.dump|exists\.db|named\.db|space\.db|legacy\.db)"), rb"/\1"),
    (re.compile(rb"^usage: [^ ]*/mdbx_[a-z]+ "), rb"usage: mdbx_BIN "),
    (re.compile(rb"^[^ ]*/mdbx_(load|dump|copy|chk|stat|drop|defrag): "), rb"mdbx_\1: "),
    (re.compile(rb"read meta: empty file \([0-9]+, .*\)"), rb"read meta: empty file (ERRNO)"),
    (re.compile(rb"^  Max readers: [0-9]+$"), rb"  Max readers: <N>"),
]

NORMALIZE_DROP = [
    re.compile(rb"troika:"),
    re.compile(rb"MADV_"),
    re.compile(rb"^   readahead ON "),
    re.compile(rb"^   skip update meta[0-9]+ for txn#[0-9]+, since it is already steady$"),
    re.compile(rb"try growth datafile"),
    re.compile(rb"dirty-entries"),
    re.compile(rb"ignore .*local/remote-fs check"),
    re.compile(rb"^[a-zA-Z_][a-zA-Z0-9_]*:[0-9]+ DIAG "),
]


def normalize(raw: bytes):
    """Split into lines and apply the volatile-field scrubber."""
    lines = raw.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()  # the trailing-newline artifact, not a line
    out = []
    for line in lines:
        if any(rx.search(line) for rx in NORMALIZE_DROP):
            continue
        for rx, repl in NORMALIZE_SUBS:
            line = rx.sub(lambda m: m.expand(repl), line)
        out.append(line)
    return out


# ---------------------------------------------------------------------------
# The golden matcher: exact dynamic-programming alignment of the golden
# template against the actual output, with the line-marker semantics from
# the module docstring.

BLOCK = b"..."
PATTERN_PREFIX = b"~ "


def is_block(line: bytes) -> bool:
    return line == BLOCK


def is_pattern(line: bytes) -> bool:
    return line.startswith(PATTERN_PREFIX)


def inline_match(pattern: bytes, s: bytes) -> bool:
    """A pattern-line body against one actual line: the '...' runs stand for
    any character runs, everything else must match literally, in order."""
    if b"..." not in pattern:
        return pattern == s
    parts = pattern.split(b"...")
    if s[: len(parts[0])] != parts[0]:
        return False
    pos = len(parts[0])
    for i in range(1, len(parts)):
        seg = parts[i]
        if i == len(parts) - 1:  # the last segment is a suffix
            if len(s) - len(seg) < pos:
                return False
            return s[len(s) - len(seg):] == seg
        if seg == b"":
            continue
        idx = s.find(seg, pos)
        if idx < 0:
            return False
        pos = idx + len(seg)
    return True


def line_ok(gline: bytes, aline: bytes) -> bool:
    if is_pattern(gline):
        return inline_match(gline[len(PATTERN_PREFIX):], aline)
    return gline == aline


def golden_align(golden, actual):
    """The exact DP alignment under the marker semantics.

    Returns (matched, diagnostics, ch) where ch[i][j] encodes how the
    position was reached (1 = literal/pattern consumed actual j-1,
    2 = block consumed actual j-1, 3 = block consumed nothing); the
    diagnostics is "" on match.
    """
    m, n = len(golden), len(actual)
    dp = [[False] * (n + 1) for _ in range(m + 1)]
    ch = [[0] * (n + 1) for _ in range(m + 1)]
    dp[0][0] = True
    for i in range(1, m + 1):
        block = is_block(golden[i - 1])
        for j in range(0, n + 1):
            if block:
                if dp[i - 1][j]:
                    dp[i][j], ch[i][j] = True, 3
                elif j > 0 and dp[i][j - 1]:
                    dp[i][j], ch[i][j] = True, 2
            elif j > 0 and dp[i - 1][j - 1] and line_ok(golden[i - 1], actual[j - 1]):
                dp[i][j], ch[i][j] = True, 1
    if dp[m][n]:
        return True, "", ch
    bi, bj = 0, 0
    for i in range(m + 1):
        for j in range(n + 1):
            if dp[i][j] and (i > bi or (i == bi and j > bj)):
                bi, bj = i, j
    diag = [f"golden_match: divergence at golden line {bi + 1}/{m} vs actual line {min(bj + 1, n)}/{n}"]
    if bi < m:
        g = "\"...\" (block wildcard)" if is_block(golden[bi]) else f"\"{golden[bi].decode('utf-8', 'replace')}\""
        diag.append(f"  golden {bi + 1}: {g}")
    if bj < n:
        diag.append(f"  actual {bj + 1}: \"{actual[bj].decode('utf-8', 'replace')}\"")
    elif bi < m:
        diag.append("  actual: <end of output>")
    return False, "\n".join(diag), ch


def golden_match(golden, actual):
    """Compare line lists under the marker semantics.

    Returns (True, "") on match; (False, diagnostics) otherwise, where the
    diagnostics names the furthest position the golden template reaches in
    the actual output before diverging.
    """
    ok, diag, _ = golden_align(golden, actual)
    return ok, diag


def golden_update(golden_path: Path, actual_lines):
    """Rewrite the golden file with the new normalized actual output, keeping
    the allowance markers aligned with the old golden. Falls back to a plain
    rewrite (and says so on stderr) when the new output no longer matches the
    old golden even with its markers."""
    old = golden_path.read_bytes().split(b"\n")
    if old and old[-1] == b"":
        old.pop()
    ok, _, ch = golden_align(old, actual_lines)
    if not ok:
        print(f"golden_update: {golden_path} does not match the new output with its markers; "
              f"markers are dropped, the golden is rewritten plain", file=sys.stderr)
        new = list(actual_lines)
    else:
        m, n = len(old), len(actual_lines)
        new = [None] * m
        i, j = m, n
        # Walk the alignment backward; one output line per golden line.
        # Blocks collapse to a single "..."; literals take the new actual
        # text; pattern lines are kept verbatim (they still matched, so their
        # authored "..." wildcards stay as the author wrote them).
        while i > 0 or j > 0:
            if i > 0 and ch[i][j] == 3:
                new[i - 1] = BLOCK
                i -= 1
            elif i > 0 and j > 0 and ch[i][j] == 1:
                new[i - 1] = old[i - 1] if is_pattern(old[i - 1]) else actual_lines[j - 1]
                i -= 1
                j -= 1
            elif i > 0 and j > 0 and ch[i][j] == 2:
                new[i - 1] = BLOCK
                j -= 1
            elif j > 0:
                j -= 1  # a stray actual line inside a block run; covered
            else:
                break
    golden_path.write_bytes(b"\n".join(new) + (b"\n" if new else b""))


# ---------------------------------------------------------------------------
# The selftest: assertions over the matcher and the update-mode marker
# preservation. Matcher specs are "<expect>|<golden>|<actual>" where ";"
# separates lines within the golden and the actual.

SELFTEST_MATCH_SPECS = [
    ("ok", "a", "a"),
    ("ok", "a;b", "a;b"),
    ("fail", "a", "a;b"),
    ("fail", "a;b", "a"),
    ("ok", "", ""),
    ("fail", "", "X"),
    ("ok", "...;b", "b"),
    ("ok", "...;b", "X;Y;b"),
    ("ok", "a;...", "a;X;Y"),
    ("ok", "a;...;b", "a;X;Y;b"),
    ("ok", "a;...;b", "a;b"),
    ("fail", "a;...;b", "X;b"),
    ("fail", "a;...;b", "a;X"),
    ("ok", "~ read meta: empty file (...)", "read meta: empty file (22, No space left)"),
    ("fail", "~ read meta: empty file (...)", "read meta: empty file"),
    ("fail", "~ x ... y", "X;Y"),
    ("ok", " - Taking lock... done", " - Taking lock... done"),
    ("fail", " - Taking lock... done", " - Taking lockXX... done"),
    ("ok", "...", "anything at all;and more"),
    ("ok", "...;...;end", "one;two;end"),
]


def split_spec(text: str):
    return [x.encode() for x in text.split(";")] if text else []


def run_selftest() -> int:
    fails = 0
    tmp = Path(tempfile.mkdtemp(prefix="golden-selftest."))
    try:
        for n, (expect, golden, actual) in enumerate(SELFTEST_MATCH_SPECS, 1):
            ok, _ = golden_match(split_spec(golden), split_spec(actual))
            got = "ok" if ok else "fail"
            if got == expect:
                print(f"  selftest {n}: PASS ({expect})")
            else:
                fails += 1
                print(f"  selftest {n}: FAIL (want {expect}, got {got}) golden=[{golden}] actual=[{actual}]")
        # The update-mode marker preservation.
        g = tmp / "g.err"
        g.write_bytes(b"banner\n...\nDone.\n")
        golden_update(g, [b"banner", b"   in-core database", b"Done."])
        if g.read_bytes() == b"banner\n...\nDone.\n":
            print("  selftest 21: PASS (update keeps the markers)")
        else:
            fails += 1
            print(f"  selftest 21: FAIL, got {g.read_bytes()!r}")
        golden_update(g, [b"WHOLE", b"NEW", b"SHAPE"])
        if g.read_bytes() == b"WHOLE\nNEW\nSHAPE\n":
            print("  selftest 22: PASS (update falls back to a plain rewrite)")
        else:
            fails += 1
            print(f"  selftest 22: FAIL, got {g.read_bytes()!r}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n== golden matcher selftest: fails={fails}")
    return 0 if fails == 0 else 1


# ---------------------------------------------------------------------------
# The runner.

def log(msg: str):
    print(f"  {msg}")


class Runner:
    def __init__(self, tool: str, tool_binary: Path, work: Path, update: bool, case_filter):
        self.tool = tool
        self.bin = tool_binary
        self.work = work
        self.update = update
        self.filter = set(case_filter)
        self.load_bin = tool_binary.parent / "mdbx_load"
        self.dump_bin = tool_binary.parent / "mdbx_dump"
        self.chk_bin = tool_binary.parent / "mdbx_chk"
        self.test_bin = tool_binary.parent / "mdbx_test"
        self.passed = 0
        self.failed = 0
        self.failed_cases = []

    def wanted(self, name: str) -> bool:
        return not self.filter or name in self.filter

    def run_capture(self, argv, stdin_path=None):
        stdin_f = open(stdin_path, "rb") if stdin_path else subprocess.DEVNULL
        try:
            proc = subprocess.run([str(a) for a in argv], stdin=stdin_f,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        finally:
            if stdin_path:
                stdin_f.close()
        return proc.returncode, proc.stdout, proc.stderr

    # -- fixtures -----------------------------------------------------------
    def fresh_base(self, d: Path):
        """base.db fixture (Main + meta + duptbl) with FIXED geometry
        (64 MiB lower=upper) so dump/stat output is machine-independent."""
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        subprocess.run([self.load_bin, "-nq", "-f", str(FIXTURES / "base.dump"),
                        "-G", "67108864:67108864:0:0:4096", str(d / "base.db")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not (d / "base.db").is_file():
            sys.exit("fixture build failed")

    def fresh_space(self, d: Path):
        """Deterministic DB with a value starting with a space byte and one
        with a mid-field space (fixtures/space.dump), fixed geometry."""
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        subprocess.run([self.load_bin, "-nq", "-f", str(FIXTURES / "space.dump"),
                        "-G", "67108864:67108864:0:0:4096", str(d / "space.db")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not (d / "space.db").is_file():
            sys.exit("space fixture build failed")

    def corrupt_db(self, src: Path, dst: Path):
        """Flip a byte in an allocated data page (offset 20480 reliably trips
        mdbx_chk for the 64KB fixture DB)."""
        shutil.copyfile(src, dst)
        with open(dst, "r+b") as f:
            f.seek(20480)
            f.write(b"\xff")

    def named_base(self, d: Path):
        """Deterministic DB with a named table via mdbx_test (fixed seed)."""
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        subprocess.run([self.test_bin, "--nops=300", "--prng-seed=42", "--repeat=1",
                        f"--pathname={d / 'named.db'}", "--dont-cleanup-after",
                        "--append", "--table=+data.fixed"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not (d / "named.db").is_file():
            sys.exit("named fixture build failed")

    # -- compare / update ---------------------------------------------------
    def compare_or_update(self, name: str, rc: int, stdout: bytes, stderr: bytes) -> bool:
        d = self.work / "out" / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "stdout").write_bytes(stdout)
        (d / "stderr").write_bytes(stderr)
        actual_out = normalize(stdout)
        actual_err = normalize(stderr)
        (d / "stdout.norm").write_bytes(b"\n".join(actual_out + [b""]) if actual_out else b"")
        (d / "stderr.norm").write_bytes(b"\n".join(actual_err + [b""]) if actual_err else b"")
        if self.update:
            (GOLDEN / self.tool).mkdir(parents=True, exist_ok=True)
            golden_update(GOLDEN / self.tool / f"{name}.out", actual_out)
            golden_update(GOLDEN / self.tool / f"{name}.err", actual_err)
            (GOLDEN / self.tool / f"{name}.rc").write_text(f"{rc}\n")
            log(f"golden {self.tool}/{name} (rc={rc})")
            return True
        ok = True
        gout, gerr = [], []
        gout_p = GOLDEN / self.tool / f"{name}.out"
        gerr_p = GOLDEN / self.tool / f"{name}.err"
        if not gout_p.is_file():
            ok = False
            log(f"MISSING golden {self.tool}/{name}.out")
        else:
            gout = normalize(gout_p.read_bytes())
        if not gerr_p.is_file():
            ok = False
            log(f"MISSING golden {self.tool}/{name}.err")
        else:
            gerr = normalize(gerr_p.read_bytes())
        ok_out, _ = golden_match(gout, actual_out)
        ok_err, _ = golden_match(gerr, actual_err)
        if not ok_out or not ok_err:
            ok = False
        want_rc = rc
        rc_file = GOLDEN / self.tool / f"{name}.rc"
        if rc_file.is_file():
            want_rc = int(rc_file.read_text().strip())
        if rc != want_rc:
            ok = False
        if ok:
            self.passed += 1
            log(f"PASS {self.tool}/{name}")
        else:
            self.failed += 1
            self.failed_cases.append(f"{self.tool}/{name}")
            log(f"FAIL {self.tool}/{name} (rc={rc}, want={want_rc}); stdout diff:")
            self.print_diff(gout, actual_out)
            log("  stderr diff:")
            self.print_diff(gerr, actual_err)
        return ok

    @staticmethod
    def print_diff(golden, actual):
        shown = 0
        for line in difflib.diff_bytes(difflib.unified_diff, golden, actual, n=0):
            if line.startswith((b"---", b"+++", b"@@")):
                continue
            print("      " + line.decode("utf-8", "replace"))
            shown += 1
            if shown >= MAX_DIFF_LINES:
                break

    def run_case(self, name: str, want_rc: int, argv) -> bool:
        if not self.wanted(name):
            return True
        d = self.work / "out" / name
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        rc, stdout, stderr = self.run_capture(argv)
        return self.compare_or_update(name, rc, stdout, stderr)

    def run_case_rc_only(self, name: str, want_rc: int, argv) -> bool:
        """rc-only case: exit code is the contract, output is volatile."""
        if not self.wanted(name):
            return True
        rc, _, _ = self.run_capture(argv)
        if rc == want_rc:
            self.passed += 1
            log(f"PASS {self.tool}/{name} (rc-only)")
            return True
        self.failed += 1
        self.failed_cases.append(f"{self.tool}/{name}")
        log(f"FAIL {self.tool}/{name} (rc={rc}, want={want_rc})")
        return False


# ---------------------------------------------------------------------------
# The cases, ported 1:1 from the original bash runner (same names, same
# fixtures, same argument lists).

def build_cases(r: Runner):
    """Returns the ordered [(name, entry_fn)] list for the runner's tool."""
    cases = []

    def add(name, fn):
        cases.append((name, fn))

    def fixture_case(name, want_rc, mk_argv, fixture):
        def entry():
            d = r.work / name
            fixture(d)
            r.run_case(name, want_rc, mk_argv(d))
        add(name, entry)

    B = r.bin

    if r.tool == "chk":
        fixture_case("clean", 0, lambda d: [B, d / "base.db"], r.fresh_base)
        fixture_case("verbose", 0, lambda d: [B, "-v", d / "base.db"], r.fresh_base)

        def specific_table():
            d = r.work / "chk_s"
            r.named_base(d)
            r.run_case("specific_table", 0, [B, "-s", "data", d / "named.db"])
        add("specific_table", specific_table)

        def corrupt():
            d = r.work / "chk_c"
            r.fresh_base(d)
            r.corrupt_db(d / "base.db", d / "bad.db")
            r.run_case("corrupt", 2, [B, d / "bad.db"])
        add("corrupt", corrupt)

        def quiet_corrupt():
            d = r.work / "chk_q"
            r.fresh_base(d)
            r.corrupt_db(d / "base.db", d / "bad.db")
            r.run_case("quiet_corrupt", 2, [B, "-q", d / "bad.db"])
        add("quiet_corrupt", quiet_corrupt)

        add("usage", lambda: r.run_case("usage", 5, [B]))
        add("version", lambda: r.run_case("version", 0, [B, "-V"]))

        def ignore_order():
            d = r.work / "chk_i"
            r.named_base(d)
            r.run_case("ignore_order", 0, [B, "-i", d / "named.db"])
        add("ignore_order", ignore_order)

    elif r.tool == "copy":
        fixture_case("basic", 0, lambda d: [B, d / "base.db", d / "copy.db"], r.fresh_base)
        fixture_case("compact", 0, lambda d: [B, "-c", d / "base.db", d / "compact.db"], r.fresh_base)

        def force():
            d = r.work / "copy_f"
            r.fresh_base(d)
            (d / "exists.db").touch()
            r.run_case("force", 0, [B, "-f", d / "base.db", d / "exists.db"])
        add("force", force)

        def stdout_case():
            # tool writes the DB image to stdout; redirect it to a file, then
            # verify the produced image with mdbx_chk (its verdict is the
            # observable contract).
            d = r.work / "copy_o"
            r.fresh_base(d)
            rc, out, _ = r.run_capture([B, d / "base.db"])
            (d / "out.img").write_bytes(out)
            r.run_case("stdout", 0, [r.chk_bin, d / "out.img"])
        add("stdout", stdout_case)

        add("usage", lambda: r.run_case("usage", 1, [B]))

    elif r.tool == "defrag":
        fixture_case("quick", 0, lambda d: [B, "-1", d / "base.db"], r.fresh_base)
        fixture_case("ratio", 0, lambda d: [B, "-f", "100", d / "base.db"], r.fresh_base)
        fixture_case("timeout", 0, lambda d: [B, "-t", "2", d / "base.db"], r.fresh_base)

    elif r.tool == "drop":
        fixture_case("empty", 0, lambda d: [B, d / "base.db"], r.fresh_base)
        fixture_case("delete", 0, lambda d: [B, "-d", d / "base.db"], r.fresh_base)
        fixture_case("named", 0, lambda d: [B, "-s", "meta", d / "base.db"], r.fresh_base)

    elif r.tool == "dump":
        for name, args in (("main", []), ("all", ["-a"]), ("named", ["-s", "meta"]),
                           ("list", ["-l"]), ("print", ["-p"])):
            fixture_case(name, 0, (lambda a: lambda d: [B] + a + [d / "base.db"])(args), r.fresh_base)

        def print_space():
            # Issue #61: a field whose first byte is a space must be escaped
            # as \20 so the dump line cannot be mistaken for the concise
            # repeat marker.
            d = r.work / "dump_ps"
            r.fresh_space(d)
            r.run_case("print_space", 0, [B, "-p", d / "space.db"])
        add("print_space", print_space)

        def concise_dups():
            d = r.work / "dump_c"
            r.fresh_base(d)
            r.run_case("concise_dups", 0, [B, "-c", "-s", "duptbl", d / "base.db"])
        add("concise_dups", concise_dups)

    elif r.tool == "load":
        def roundtrip():
            d = r.work / "load_rt"
            r.fresh_base(d)
            with open(d / "out.dump", "wb") as dump_out:
                subprocess.run([r.dump_bin, "-a", str(d / "base.db")],
                               stdout=dump_out, stderr=subprocess.DEVNULL)
            r.run_case("roundtrip", 0, [B, "-nf", d / "out.dump", d / "reload.db"])
        add("roundtrip", roundtrip)

        def stdin_case():
            # Issue #50: stdin-by-default must load the dump into the dbpath
            # (the last positional arg), not mis-bind it to '-f'. The loaded
            # database must be identical to one loaded from the same stream
            # via '-f file'.
            # The FIXED geometry pins the default-mapping choice out of the
            # comparison: without it each load process derives the upper from
            # its own RAM estimate (reasonable_db_maxsize()), so on a loaded
            # runner the two processes may round to different GiB boundaries
            # and the round-trip comparison fails spuriously (observed as the
            # 23 vs 24 GiB mismatch on the 2026-11-24 nightly's profgc cell).
            pinned_geometry = ["-G", "67108864:67108864:0:0:4096"]
            d = r.work / "load_si"
            shutil.rmtree(d, ignore_errors=True)
            d.mkdir(parents=True)
            with open(FIXTURES / "base.dump", "rb") as dump_in:
                proc = subprocess.run([B, "-nf", str(d / "base.db")] + pinned_geometry,
                                      stdin=dump_in, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
            data_ok = True
            subprocess.run([r.load_bin, "-nq", "-f", str(FIXTURES / "base.dump")] + pinned_geometry
                           + [str(d / "ref.db")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            reloaded = subprocess.run([r.dump_bin, "-q", str(d / "base.db")],
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
            ref = subprocess.run([r.dump_bin, "-q", str(d / "ref.db")],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
            if normalize(reloaded) != normalize(ref):
                data_ok = False
            if r.update:
                (GOLDEN / r.tool).mkdir(parents=True, exist_ok=True)
                golden_update(GOLDEN / r.tool / "stdin.out", normalize(stdout))
                golden_update(GOLDEN / r.tool / "stdin.err", normalize(stderr))
                (GOLDEN / r.tool / "stdin.rc").write_text(f"{rc}\n")
                log(f"golden {r.tool}/stdin (rc={rc}, data_ok={int(data_ok)})")
                return
            ok = True
            aout, aerr = normalize(stdout), normalize(stderr)
            gout = normalize((GOLDEN / r.tool / "stdin.out").read_bytes())
            gerr = normalize((GOLDEN / r.tool / "stdin.err").read_bytes())
            if golden_match(gout, aout)[0] is False or golden_match(gerr, aerr)[0] is False:
                ok = False
            want_rc = int((GOLDEN / r.tool / "stdin.rc").read_text().strip())
            if rc != want_rc:
                ok = False
            if not data_ok:
                ok = False
                log("  load/stdin: loaded data mismatch (issue #50 regression)")
                r.print_diff(normalize(ref), normalize(reloaded))
            if ok:
                r.passed += 1
                log(f"PASS {r.tool}/stdin")
            else:
                r.failed += 1
                r.failed_cases.append(f"{r.tool}/stdin")
                log(f"FAIL {r.tool}/stdin (rc={rc} want={want_rc})")
                log("  stdout diff:")
                r.print_diff(gout, aout)
                log("  stderr diff:")
                r.print_diff(gerr, aerr)
        add("stdin", stdin_case)

        def leading_space():
            # Issue #61: a print-mode dump escapes a leading space as \20, and
            # the round-trip must preserve the data exactly (including raw
            # mid-field spaces).
            d = r.work / "load_ls"
            r.fresh_space(d)
            with open(d / "print.dump", "wb") as dump_out:
                subprocess.run([r.dump_bin, "-p", str(d / "space.db")],
                               stdout=dump_out, stderr=subprocess.DEVNULL)
            proc = subprocess.run([B, "-nf", str(d / "print.dump"), str(d / "reload.db")],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
            data_ok = True
            src = subprocess.run([r.dump_bin, "-q", str(d / "space.db")],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
            reloaded = subprocess.run([r.dump_bin, "-q", str(d / "reload.db")],
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
            if src != reloaded:
                data_ok = False
            if r.update:
                (GOLDEN / r.tool).mkdir(parents=True, exist_ok=True)
                golden_update(GOLDEN / r.tool / "leading_space.out", normalize(stdout))
                golden_update(GOLDEN / r.tool / "leading_space.err", normalize(stderr))
                (GOLDEN / r.tool / "leading_space.rc").write_text(f"{rc}\n")
                log(f"golden {r.tool}/leading_space (rc={rc}, data_ok={int(data_ok)})")
                return
            ok = True
            if not (GOLDEN / r.tool / "leading_space.out").is_file():
                ok = False
                log(f"MISSING golden {r.tool}/leading_space.out")
            aout, aerr = normalize(stdout), normalize(stderr)
            gout = normalize((GOLDEN / r.tool / "leading_space.out").read_bytes())
            gerr = normalize((GOLDEN / r.tool / "leading_space.err").read_bytes())
            if golden_match(gout, aout)[0] is False or golden_match(gerr, aerr)[0] is False:
                ok = False
            want_rc = int((GOLDEN / r.tool / "leading_space.rc").read_text().strip())
            if rc != want_rc:
                ok = False
            if not data_ok:
                ok = False
                log("  load/leading_space: round-trip data mismatch")
                r.print_diff(src.splitlines(), reloaded.splitlines())
            if ok:
                r.passed += 1
                log(f"PASS {r.tool}/leading_space")
            else:
                r.failed += 1
                r.failed_cases.append(f"{r.tool}/leading_space")
                log(f"FAIL {r.tool}/leading_space (rc={rc} want={want_rc})")
                log("  stdout diff:")
                r.print_diff(gout, aout)
                log("  stderr diff:")
                r.print_diff(gerr, aerr)
        add("leading_space", leading_space)

        def legacy_two_space():
            # Issue #61: a pre-fix print-mode dump writes a raw two-space line
            # for a leading-space field; mdbx_load must reject it as a
            # concise-repeat marker where a value is expected (parse error,
            # non-zero exit).
            d = r.work / "load_l2"
            shutil.rmtree(d, ignore_errors=True)
            d.mkdir(parents=True)
            (d / "legacy.dump").write_bytes(
                b"VERSION=3\nformat=print\ntype=btree\ndb_pagesize=4096\nHEADER=END\n"
                b" k1\n AAAA\n k2\n  LEGACY_SPACE_VALUE\nDATA=END\n")
            r.run_case("legacy_two_space", 1, [B, "-nf", d / "legacy.dump", d / "legacy.db"])
        add("legacy_two_space", legacy_two_space)

        def bad_mapsize():
            d = r.work / "load_bm"
            r.fresh_base(d)
            (d / "bad.dump").write_bytes(
                b"VERSION=3\nformat=bytevalue\ntype=btree\nmapsize=99999999999999999999\nHEADER=END\nDATA=END\n")
            r.run_case("bad_mapsize", 1, [B, "-nf", d / "bad.dump", d / "bad.db"])
        add("bad_mapsize", bad_mapsize)

        def bad_version():
            d = r.work / "load_bv"
            d.mkdir(parents=True, exist_ok=True)
            (d / "ver.dump").write_bytes(b"VERSION=2\nHEADER=END\nDATA=END\n")
            r.run_case("bad_version", 1, [B, "-nf", d / "ver.dump", d / "ver.db"])
        add("bad_version", bad_version)

        def purge():
            d = r.work / "load_p"
            r.fresh_base(d)
            r.run_case("purge", 0, [B, "-pnf", FIXTURES / "base.dump", d / "base.db"])
        add("purge", purge)

    elif r.tool == "stat":
        fixture_case("main", 0, lambda d: [B, d / "base.db"], r.fresh_base)
        fixture_case("envinfo", 0, lambda d: [B, "-e", d / "base.db"], r.fresh_base)
        fixture_case("all", 0, lambda d: [B, "-a", d / "base.db"], r.fresh_base)
        fixture_case("named", 0, lambda d: [B, "-s", "meta", d / "base.db"], r.fresh_base)

        def readers():
            # reader table contains live pid/thread/txnid -> rc-only contract
            d = r.work / "stat_r"
            r.fresh_base(d)
            r.run_case_rc_only("readers", 0, [B, "-r", d / "base.db"])
        add("readers", readers)

        fixture_case("gc", 0, lambda d: [B, "-f", d / "base.db"], r.fresh_base)

    # -h / --help (all tools)
    add("help", lambda: r.run_case("help", 0, [B, "-h"]))
    add("help_long", lambda: r.run_case("help_long", 0, [B, "--help"]))
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1] if __doc__ else "")
    parser.add_argument("--selftest", action="store_true",
                        help="run the golden-matcher assertions, no fixtures/tools")
    parser.add_argument("tool", nargs="?", help="chk|copy|defrag|drop|dump|load|stat")
    parser.add_argument("tool_binary", nargs="?", help="path to the tool binary")
    parser.add_argument("workdir", nargs="?", help="scratch dir (removed on any exit path)")
    parser.add_argument("--update", action="store_true", help="re-write goldens instead of comparing")
    parser.add_argument("cases", nargs="*", help="run only the listed cases (default: all)")
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()
    if not args.tool or not args.tool_binary or not args.workdir:
        parser.error("tool, tool-binary and workdir are required")
    if args.tool not in ("chk", "copy", "defrag", "drop", "dump", "load", "stat"):
        parser.error(f"unknown tool: {args.tool}")

    work = Path(args.workdir)
    runner = Runner(args.tool, Path(args.tool_binary), work, args.update, args.cases)
    try:
        for _name, entry in build_cases(runner):
            entry()
    finally:
        # Remove the whole runner worktree on ANY exit path: each case deletes
        # its directory before the run, but ~44 fixture databases (about
        # 2.7 GB) would otherwise accumulate in the build tree on every run.
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n== {args.tool}: pass={runner.passed} fail={runner.failed}")
    if runner.failed != 0:
        print("failed: " + ", ".join(runner.failed_cases))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

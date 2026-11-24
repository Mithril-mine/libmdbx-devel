/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "../env-guard.h++"
#include "../issues/tool_runner.h"
#include "../test-env.h++"
#include "mdbx.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

/* The deterministic mdbx_dump -> mdbx_load -> mdbx_copy round-trip family
 * (issue #552, verification-campaign item SC-97): a shape x seed matrix of
 * table shapes (integer-keyed, printable strings, dupsort multis, dupfixed,
 * overflow-page values, empty keys/values, keys with embedded NULs and high
 * bytes), each seed perturbing keys, values and the duplicates distribution.
 * Every seed runs the full round trip:
 *   - fill a database through the public API;
 *   - mdbx_dump -n (raw bytevalue) -> mdbx_load from the dump file;
 *   - mdbx_dump -> mdbx_load through real stdin/stdout pipes (the stdio path
 *     where load-nul-byte-silent-truncation and its silent-swallow siblings
 *     lived);
 *   - mdbx_copy -f (plain) and mdbx_copy -c (compacting);
 *   - byte-exact full cursor scans comparing the source content against every
 *     reloaded/copied database (exact comparison, not a checksum);
 *   - exit 0 and zero stderr noise asserted for every tool invocation, so a
 *     silent truncation warning cannot hide behind a successful exit code.
 * Deterministic: std::mt19937 with fixed per-(shape, seed) seeds, no clocks -
 * any failure reproduces exactly. POSIX-only (pipes and fork), registered
 * alongside the ut_tools corpus under UNIX && MDBX_BUILD_TOOLS. */

namespace {

constexpr unsigned kSeeds = 8;

enum class shape_id { ints, strings, multis, dupfixed, overflow, empties, binary };

struct rec {
  std::vector<uint8_t> key, val;
};

static std::string hex_view(const std::vector<uint8_t> &v) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  const size_t n = std::min<size_t>(v.size(), 16);
  for (size_t i = 0; i < n; ++i) {
    out += digits[v[i] >> 4];
    out += digits[v[i] & 15];
  }
  if (v.size() > n)
    out += "...";
  return out;
}

/* Tools are taken from the environment (the registration site passes the
 * $<TARGET_FILE:...> paths, the gh0485 pattern) or, when unset, looked up
 * next to the running test executable. */
static std::string tool_from_env_or_sibling(const char *env_name, const char *tool) {
  const char *via_env = getenv(env_name);
  return (via_env && *via_env) ? std::string(via_env) : tool_runner::tool_path(tool);
}

static const std::string dump_tool = tool_from_env_or_sibling("MDBX_TOOL_DUMP", "mdbx_dump");
static const std::string load_tool = tool_from_env_or_sibling("MDBX_TOOL_LOAD", "mdbx_load");
static const std::string copy_tool = tool_from_env_or_sibling("MDBX_TOOL_COPY", "mdbx_copy");

/* ------------------------------------------------------------------------ */
/* scratch artifacts: five NOSUBDIR databases and the plain pipeline files  */

struct roundtrip_names {
  std::string src, rlf, rlp, cpf, cpc;
  std::string dump, pipe_stdout, err_dump_f, err_dump_p, err_load_f, err_load_p, err_copy_f, err_copy_c;

  explicit roundtrip_names(const char *prefix) {
    src = std::string(prefix) + "-src.mdbx";
    rlf = std::string(prefix) + "-reload-file.mdbx";
    rlp = std::string(prefix) + "-reload-pipe.mdbx";
    cpf = std::string(prefix) + "-copy-plain.mdbx";
    cpc = std::string(prefix) + "-copy-compact.mdbx";
    dump = std::string(prefix) + "-dump.raw";
    pipe_stdout = std::string(prefix) + "-load.stdout";
    err_dump_f = std::string(prefix) + "-dump-file.stderr";
    err_dump_p = std::string(prefix) + "-dump-pipe.stderr";
    err_load_f = std::string(prefix) + "-load-file.stderr";
    err_load_p = std::string(prefix) + "-load-pipe.stderr";
    err_copy_f = std::string(prefix) + "-copy-plain.stderr";
    err_copy_c = std::string(prefix) + "-copy-compact.stderr";
  }

  std::vector<const std::string *> env_paths() const { return {&src, &rlf, &rlp, &cpf, &cpc}; }
  std::vector<const std::string *> file_paths() const {
    return {&dump, &pipe_stdout, &err_dump_f, &err_dump_p, &err_load_f, &err_load_p, &err_copy_f, &err_copy_c};
  }
};

/* Removes the plain (non-database) scratch files on every exit path; the
 * databases themselves are covered by the env_files_guard. */
class scratch_files_guard {
  const std::vector<std::string> paths;

  static void remove_one(const std::string &path, bool teardown) {
    if (remove(path.c_str()) != 0 && errno != ENOENT) {
      if (teardown)
        ADD_FAILURE() << "scratch_files_guard: unable to remove `" << path << "`, errno " << errno;
    }
  }

public:
  scratch_files_guard(std::initializer_list<std::string> names) : paths(names) {
    for (const auto &path : paths)
      remove_one(path, false);
  }
  ~scratch_files_guard() {
    for (const auto &path : paths)
      remove_one(path, true);
  }
};

static void wipe_scratch(const roundtrip_names &names) {
  for (const auto &path : names.env_paths())
    mdbx_env_delete(path->c_str(), MDBX_ENV_JUST_DELETE);
  for (const auto &path : names.file_paths())
    remove(path->c_str());
}

/* ------------------------------------------------------------------------ */
/* the deterministic data-shape generator                                   */

static MDBX_db_flags_t shape_flags(shape_id shape) {
  switch (shape) {
  case shape_id::ints:
    return MDBX_INTEGERKEY;
  case shape_id::multis:
    return MDBX_DUPSORT;
  case shape_id::dupfixed:
    return MDBX_DUPSORT | MDBX_DUPFIXED;
  default:
    return MDBX_db_flags_t(0);
  }
}

/* The byte-lexicographic order of libmdbx's default comparator: memcmp over
 * the common prefix, then the length. The explicit comparison avoids the
 * GCC-13 -Wstringop-overread false positive, which the synthesized
 * vector<uint8_t> spaceship triggers by misestimating the bound of its
 * __builtin_memcmp (an out-of-range [2^63, 2^64) length). */
static int bytes_lexical_cmp(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  const size_t common = std::min(a.size(), b.size());
  const int rc = common ? std::memcmp(a.data(), b.data(), common) : 0;
  if (rc)
    return rc;
  return (a.size() < b.size()) ? -1 : (a.size() > b.size()) ? +1 : 0;
}

static void sort_records(shape_id shape, std::vector<rec> &recs) {
  if (shape == shape_id::ints) {
    /* the integerkey comparator: native-order uint32 keys of equal size */
    std::sort(recs.begin(), recs.end(), [](const rec &a, const rec &b) {
      uint32_t ka = 0, kb = 0;
      memcpy(&ka, a.key.data(), sizeof(ka));
      memcpy(&kb, b.key.data(), sizeof(kb));
      if (ka != kb)
        return ka < kb;
      return bytes_lexical_cmp(a.val, b.val) < 0;
    });
    return;
  }
  std::sort(recs.begin(), recs.end(), [](const rec &a, const rec &b) {
    const int rc = bytes_lexical_cmp(a.key, b.key);
    return rc ? rc < 0 : bytes_lexical_cmp(a.val, b.val) < 0;
  });
}

static std::vector<rec> generate_records(shape_id shape, unsigned seed) {
  std::mt19937 rng(UINT32_C(0x51ED270B) + seed * UINT32_C(0x9E3779B9) + 104729u * static_cast<unsigned>(shape));
  auto draw = [&rng]() -> unsigned { return static_cast<unsigned>(rng() & 0xffffffffu); };
  /* seed 0 is the empty database; seeds 1-3 are tiny; the rest are medium
   * (the overflow shape guarantees big multi-page values there) */
  const unsigned count = (seed == 0) ? 0u : (seed < 4u) ? 6u + draw() % 36u : 140u + draw() % 120u;
  std::vector<rec> recs;
  recs.reserve(count * 2u);
  char buf[48];
  uint32_t ints_key = UINT32_C(0x40000000);
  for (unsigned i = 0; i < count; ++i) {
    rec r;
    bool dup_only = false;
    switch (shape) {
    case shape_id::ints: {
      /* strictly increasing keys: a random per-record step accumulates */
      ints_key += 1u + draw() % 3u;
      r.key.resize(sizeof(ints_key));
      memcpy(r.key.data(), &ints_key, sizeof(ints_key));
      r.val.resize(draw() % 64u);
      for (auto &b : r.val)
        b = static_cast<uint8_t>(draw() & 0xff);
      break;
    }
    case shape_id::strings: {
      snprintf(buf, sizeof(buf), "k%06u-%08x", i, draw());
      r.key.assign(buf, buf + strlen(buf));
      r.val.resize(4u + draw() % 96u);
      for (auto &b : r.val)
        b = static_cast<uint8_t>('a' + draw() % 26u);
      break;
    }
    case shape_id::multis: {
      snprintf(buf, sizeof(buf), "m%06u", i);
      r.key.assign(buf, buf + strlen(buf));
      const unsigned dups = 1u + draw() % ((seed < 4u) ? 3u : 6u);
      for (unsigned d = 0; d < dups; ++d) {
        rec dup;
        dup.key = r.key;
        if (d == 0u && draw() % 5u == 0u) {
          /* an empty duplicate, at most one per key since (key, "") is unique */
          dup.val.clear();
        } else {
          snprintf(buf, sizeof(buf), "d%03u-%08x", d, draw());
          dup.val.assign(buf, buf + strlen(buf));
          /* keep at least the `d%03u` part, so duplicates stay unique */
          dup.val.resize(4u + draw() % std::max<size_t>(1u, dup.val.size() - 4u));
        }
        recs.push_back(std::move(dup));
      }
      dup_only = true;
      break;
    }
    case shape_id::dupfixed: {
      snprintf(buf, sizeof(buf), "f%06u", i);
      r.key.assign(buf, buf + strlen(buf));
      const unsigned dups = 1u + draw() % 4u;
      for (unsigned d = 0; d < dups; ++d) {
        rec dup;
        dup.key = r.key;
        /* every duplicate exactly 16 bytes long, as DUPFIXED requires */
        snprintf(buf, sizeof(buf), "%06x%08x", d, draw());
        dup.val.assign(16u, static_cast<uint8_t>('p'));
        memcpy(dup.val.data(), buf, strlen(buf));
        recs.push_back(std::move(dup));
      }
      dup_only = true;
      break;
    }
    case shape_id::overflow: {
      snprintf(buf, sizeof(buf), "o%06u", i);
      r.key.assign(buf, buf + strlen(buf));
      /* every fourth medium record spans overflow pages at any CI page size */
      const bool big = (seed >= 4u) && (i % 4u == 0u);
      r.val.resize(big ? (32u << 10) + draw() % 16384u : 8u + draw() % 600u);
      for (size_t j = 0; j < r.val.size(); ++j)
        r.val[j] = static_cast<uint8_t>('a' + (j * 7u + i) % 26u);
      break;
    }
    case shape_id::empties: {
      if (i == 0u)
        r.key.clear(); /* the only zero-length key possible */
      else {
        snprintf(buf, sizeof(buf), "e%06u", i);
        r.key.assign(buf, buf + strlen(buf));
      }
      if (i % 3u == 0u)
        r.val.clear();
      else {
        r.val.resize(2u + draw() % 40u);
        for (auto &b : r.val)
          b = static_cast<uint8_t>('a' + draw() % 26u);
      }
      break;
    }
    case shape_id::binary: {
      static const uint8_t specials[] = {0x00, 0x01, 0x09, 0x1F, 0x7F, 0x80, 0xC0, 0xFE, 0xFF};
      snprintf(buf, sizeof(buf), "b%06u", i);
      r.key.assign(buf, buf + strlen(buf));
      const unsigned tail = 1u + draw() % 5u;
      for (unsigned t = 0; t < tail; ++t)
        r.key.push_back((draw() % 2u == 0u) ? specials[draw() % (sizeof(specials) / sizeof(specials[0]))]
                                            : static_cast<uint8_t>(draw() & 0xff));
      if (draw() % 5u == 0u) {
        r.val.clear();
      } else {
        r.val.resize(1u + draw() % 200u);
        for (auto &b : r.val)
          b = static_cast<uint8_t>(draw() & 0xff);
      }
      break;
    }
    }
    if (dup_only)
      continue; /* the multis/dupfixed records were pushed per-duplicate */
    /* zero-length keys/values are legal content; only a record that would be
     * entirely empty (which happens nowhere but the empties shape) is skipped */
    if (!r.key.empty() || !r.val.empty() || shape == shape_id::empties)
      recs.push_back(std::move(r));
  }
  sort_records(shape, recs);
  return recs;
}

/* ------------------------------------------------------------------------ */
/* filling and scanning the databases                                       */

static void create_db(const char *path, shape_id shape, const std::vector<rec> &recs) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  /* the issue_gh0058/59/71/72 idiom: an explicit modest upper bound, since
   * the default ~8 GiB geometry cannot be mapped by the spawned tools in the
   * ASan/Valgrind cells; the dump carries it to mdbx_load as the geometry
   * line, so the reloaded databases inherit it and no -G override is needed */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 64 << 20, -1, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, shape_flags(shape) | MDBX_CREATE, &dbi));
  for (const rec &r : recs) {
    MDBX_val key = {(void *)r.key.data(), r.key.size()};
    MDBX_val data = {(void *)r.val.data(), r.val.size()};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT)) << "put failed at key " << hex_view(r.key);
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
}

static rec take_rec(const MDBX_val &k, const MDBX_val &v) {
  rec r;
  if (k.iov_len) {
    const uint8_t *kb = static_cast<const uint8_t *>(k.iov_base);
    r.key.assign(kb, kb + k.iov_len);
  }
  if (v.iov_len) {
    const uint8_t *vb = static_cast<const uint8_t *>(v.iov_base);
    r.val.assign(vb, vb + v.iov_len);
  }
  return r;
}

/* Full cursor scan of a whole (single-table) environment, duplicates
 * flattened in the stored order. EXPECT-only: usable from a value-returning
 * helper; a broken scan yields an empty vector and the comparison reports. */
static std::vector<rec> scan_db(const char *path) {
  std::vector<rec> recs;
  MDBX_env *env = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path, MDBX_RDONLY, 0));
  MDBX_txn *txn = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_dbi dbi = 0;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_ACCEDE, &dbi));
  unsigned flags = 0;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_dbi_flags(txn, dbi, &flags));
  const bool dupsort = (flags & MDBX_DUPSORT) != 0;
  MDBX_cursor *cursor = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
  MDBX_val key, data;
  int rc = mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST);
  while (rc == MDBX_SUCCESS) {
    recs.push_back(take_rec(key, data));
    rc = mdbx_cursor_get(cursor, &key, &data, dupsort ? MDBX_NEXT_DUP : MDBX_NEXT);
    if (dupsort && rc == MDBX_NOTFOUND)
      rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT);
  }
  EXPECT_EQ(MDBX_NOTFOUND, rc) << "cursor walk of `" << path << "` failed, rc " << rc;
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  return recs;
}

static void expect_same_content(const char *label, const std::vector<rec> &origin, const std::vector<rec> &other) {
  ASSERT_EQ(origin.size(), other.size()) << label << ": the entry count differs";
  for (size_t i = 0; i < origin.size(); ++i) {
    if (origin[i].key != other[i].key) {
      ADD_FAILURE() << label << ": key of entry " << i << " differs: " << hex_view(origin[i].key) << " vs "
                    << hex_view(other[i].key);
      return;
    }
    if (origin[i].val != other[i].val) {
      ADD_FAILURE() << label << ": value of entry " << i << " (key " << hex_view(origin[i].key) << ") differs, "
                    << origin[i].val.size() << "B vs " << other[i].val.size() << "B";
      return;
    }
  }
}

/* ------------------------------------------------------------------------ */
/* running the tools                                                        */

static std::string joined_args(const std::vector<std::string> &args) {
  std::string out;
  for (const auto &a : args) {
    if (!out.empty())
      out += ' ';
    out += a;
  }
  return out;
}

/* In development builds (MDBX_DEBUG > 0) the library NOTICE-logs its internal
 * traces to stderr unless a logger is installed, and the quiet-mode tools
 * don't install one. Such lines are environment noise, not the tools'
 * contract (the golden_runner allows them at marked positions): every library
 * trace carries the `function:line ` (or bare `line: `) prefix, so a stderr
 * line without it can only be the tool's own diagnostic - a failure here.
 * Release builds log nothing, so the assertion is strict there. */
static bool is_library_trace_line(const std::string &line) {
  size_t name = 0;
  while (name < line.size() && (isalnum(static_cast<unsigned char>(line[name])) || line[name] == '_'))
    ++name;
  if (name == 0) {
    /* the no-function form of the debug_log() prefix */
    size_t digits = 0;
    while (digits < line.size() && isdigit(static_cast<unsigned char>(line[digits])) != 0)
      ++digits;
    return digits > 0 && digits < line.size() && line[digits] == ' ';
  }
  size_t digits = name + 1;
  while (digits < line.size() && isdigit(static_cast<unsigned char>(line[digits])) != 0)
    ++digits;
  return digits > name + 1 && digits < line.size() && line[digits] == ' ';
}

static void expect_stderr_quiet(const char *label, const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t begin = 0;
  while (begin < content.size()) {
    size_t end = content.find('\n', begin);
    if (end == std::string::npos)
      end = content.size();
    const std::string line = content.substr(begin, end - begin);
    if (!line.empty() && !is_library_trace_line(line)) {
      ADD_FAILURE() << label << " wrote to stderr: `" << line << "`";
      return;
    }
    begin = end + 1;
  }
}

static void run_tool_quietly(const char *label, const std::vector<std::string> &args, const std::string &stderr_path) {
  const int rc = tool_runner::run(args, nullptr, nullptr, stderr_path.c_str());
  ASSERT_EQ(0, rc) << "`" << joined_args(args) << "` exited " << rc;
  expect_stderr_quiet(label, stderr_path);
}

/* child-side redirection for the pipeline below; the children exec at once,
 * so nothing else of the parent's state leaks into them */
static void child_redirect(int target_fd, const std::string &path) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0 || dup2(fd, target_fd) < 0)
    _exit(126);
  close(fd);
}

/* mdbx_dump with its stdout wired to a real pipe feeding mdbx_load's stdin:
 * the stdio pass-through path (printf -> pipe -> fgets/fgetc) in which the
 * campaign's load defects lived. Both exit codes must be zero and both
 * stderr captures empty. */
static void run_pipeline(const std::vector<std::string> &producer_args, const std::vector<std::string> &consumer_args,
                         const std::string &producer_stderr, const std::string &consumer_stdout,
                         const std::string &consumer_stderr) {
  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  std::vector<std::string> producer = producer_args, consumer = consumer_args;
  std::vector<char *> producer_argv, consumer_argv;
  for (auto &a : producer)
    producer_argv.push_back(&a[0]);
  for (auto &a : consumer)
    consumer_argv.push_back(&a[0]);
  producer_argv.push_back(nullptr);
  consumer_argv.push_back(nullptr);

  const pid_t pid_write = fork();
  ASSERT_NE(-1, pid_write);
  if (pid_write == 0) {
    while (dup2(fds[1], STDOUT_FILENO) < 0 && errno == EINTR) {
    }
    close(fds[0]);
    close(fds[1]);
    child_redirect(STDERR_FILENO, producer_stderr);
    execvp(producer_argv[0], producer_argv.data());
    _exit(127);
  }
  const pid_t pid_read = fork();
  ASSERT_NE(-1, pid_read);
  if (pid_read == 0) {
    while (dup2(fds[0], STDIN_FILENO) < 0 && errno == EINTR) {
    }
    close(fds[0]);
    close(fds[1]);
    child_redirect(STDOUT_FILENO, consumer_stdout);
    child_redirect(STDERR_FILENO, consumer_stderr);
    execvp(consumer_argv[0], consumer_argv.data());
    _exit(127);
  }
  close(fds[0]);
  close(fds[1]);

  int status = 0;
  ASSERT_EQ(pid_write, waitpid(pid_write, &status, 0));
  const int rc_write = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  status = 0;
  ASSERT_EQ(pid_read, waitpid(pid_read, &status, 0));
  const int rc_read = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

  ASSERT_EQ(0, rc_write) << "`" << joined_args(producer_args) << "` (pipe producer) exited " << rc_write;
  ASSERT_EQ(0, rc_read) << "`" << joined_args(consumer_args) << "` (pipe consumer) exited " << rc_read;
  expect_stderr_quiet("mdbx_dump (pipe producer)", producer_stderr);
  expect_stderr_quiet("mdbx_load (pipe consumer)", consumer_stderr);
}

/* ------------------------------------------------------------------------ */
/* the round trip itself                                                    */

static void roundtrip_case(shape_id shape, const roundtrip_names &names) {
  for (unsigned seed = 0; seed < kSeeds; ++seed) {
    wipe_scratch(names);
    const std::vector<rec> expected = generate_records(shape, seed);
    create_db(names.src.c_str(), shape, expected);

    const std::vector<rec> origin = scan_db(names.src.c_str());
    expect_same_content("generated records vs source scan", expected, origin);

    /* dump -> dump file -> load */
    run_tool_quietly("mdbx_dump", {dump_tool, "-q", "-n", "-f", names.dump, names.src}, names.err_dump_f);
    run_tool_quietly("mdbx_load", {load_tool, "-q", "-n", "-f", names.dump, names.rlf}, names.err_load_f);

    /* dump -> stdout pipe -> load stdin */
    run_pipeline({dump_tool, "-q", "-n", names.src}, {load_tool, "-q", "-n", names.rlp}, names.err_dump_p,
                 names.pipe_stdout, names.err_load_p);

    /* copies: forced plain and compacting */
    run_tool_quietly("mdbx_copy", {copy_tool, "-q", "-n", "-f", names.src, names.cpf}, names.err_copy_f);
    run_tool_quietly("mdbx_copy", {copy_tool, "-q", "-n", "-c", names.src, names.cpc}, names.err_copy_c);

    expect_same_content("reload from dump file vs source", origin, scan_db(names.rlf.c_str()));
    expect_same_content("reload through stdin/stdout pipes vs source", origin, scan_db(names.rlp.c_str()));
    expect_same_content("plain copy vs source", origin, scan_db(names.cpf.c_str()));
    expect_same_content("compacting copy vs source", origin, scan_db(names.cpc.c_str()));
  }
}

static void run_roundtrip_test(shape_id shape, const char *prefix) {
  const roundtrip_names names(prefix);
  const env_files_guard env_guard(
      {names.src.c_str(), names.rlf.c_str(), names.rlp.c_str(), names.cpf.c_str(), names.cpc.c_str()});
  const scratch_files_guard files_guard({names.dump, names.pipe_stdout, names.err_dump_f, names.err_dump_p,
                                         names.err_load_f, names.err_load_p, names.err_copy_f, names.err_copy_c});
  roundtrip_case(shape, names);
}

} // namespace

TEST(tools_roundtrip, ints) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  run_roundtrip_test(shape_id::ints, "rtf-ints");
}

TEST(tools_roundtrip, strings) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  run_roundtrip_test(shape_id::strings, "rtf-strings");
}

TEST(tools_roundtrip, multis) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  run_roundtrip_test(shape_id::multis, "rtf-multis");
}

TEST(tools_roundtrip, dupfixed) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  run_roundtrip_test(shape_id::dupfixed, "rtf-dupfixed");
}

TEST(tools_roundtrip, overflow) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  run_roundtrip_test(shape_id::overflow, "rtf-overflow");
}

TEST(tools_roundtrip, empties) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  run_roundtrip_test(shape_id::empties, "rtf-empties");
}

TEST(tools_roundtrip, binary) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  run_roundtrip_test(shape_id::binary, "rtf-binary");
}

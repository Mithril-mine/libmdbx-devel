#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/116
// debug_log_va() passed the vsnprintf() return value to the nofmt logger as
// the message length without checking the buffer size: on POSIX a message
// longer than the buffer made the callback read past the end of the
// application's memory; on Windows it was silently dropped. The truncated
// message is now terminated and delivered with the length of what fits.

static size_t received_length;
static char received_msg[32];

static void nofmt_logger(MDBX_log_level_t log_level, const char *function, int line, const char *msg,
                         unsigned int length) noexcept {
  (void)log_level;
  (void)function;
  (void)line;
  received_length = length;
  if (length < sizeof(received_msg))
    memcpy(received_msg, msg, length);
}

TEST(issue_gh0116, nofmt_logger_receives_truncated_bounded_message) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  char buffer[16];
  received_length = 0;
  memset(received_msg, 0, sizeof(received_msg));

  /* Install the nofmt logger with a 16-byte application buffer. */
  ::mdbx_setup_debug_nofmt(MDBX_LOG_DEBUG, MDBX_DBG_DONTCHANGE, nofmt_logger, buffer, sizeof(buffer));

  /* The library formats the message through the same path the issue reports:
   * a message that does not fit the user buffer. Force it by opening a plain
   * non-database file as an environment, which logs a long error. */
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  (void)mdbx_env_open(env, "issue116-notadb.mdbx", MDBX_ENV_DEFAULTS, 0644);
  mdbx_env_close(env);

  /* The message must actually reach the nofmt logger: on Windows the old code
   * dropped an over-long message, leaving length 0 and passing every check. */
  ASSERT_GT(received_length, 0u) << "the logger must receive the (truncated) message";
  EXPECT_LE(received_length, sizeof(buffer) - 1)
      << "the delivered length must fit the application buffer, not the full message size";
  EXPECT_EQ(0, buffer[received_length]) << "the delivered message must be terminated";
}
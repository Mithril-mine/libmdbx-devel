/*
 * mprobe_test.c — простой тест трёх макросов (первая версия).
 * Проверяет: штатное поведение без инъекции; инъекцию через tier=test
 * fallback (без root, все платформы); на Windows при MPROBE_USE_DETOURS —
 * инъекцию через Microsoft Detours на WinFaultInjectHook.
 * Возвращает 0 при успехе, иначе 1.
 */
#include <stdio.h>
#include <string.h>

#include "mprobe.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                           \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      printf("FAIL: %s\n", msg);                                                   \
      g_failures++;                                                                \
    } else {                                                                       \
      printf("ok  : %s\n", msg);                                                   \
    }                                                                              \
  } while (0)

/* Типовая операция с точками COLLECT/WATCH/FAULT. */
static int do_save(const char *user_id) {
  int error_code = 0;
  MPROBE_COLLECT(save_user_start, (long)(user_id != NULL));
  MPROBE_WATCH(save_enter, 1);
  MPROBE_FAULT(inject_io_error, error_code);
  if (error_code != 0) {
    MPROBE_WATCH(save_failed, error_code);
    return error_code;
  }
  MPROBE_COLLECT(save_ok, 1);
  return 0;
}

/* tier=test реализация инъекции: для точки inject_io_error подменяем код. */
static void test_inject_hook(const char *name, void *var_ptr) {
  if (strcmp(name, "inject_io_error") == 0)
    *(int *)var_ptr = -5; /* EIO-подобный код */
}

#if defined(_WIN32) && defined(MPROBE_USE_DETOURS)
#include <windows.h>
#include <detours.h>

static void (*TrueWinFaultInjectHook)(const char *, void *) = WinFaultInjectHook;

static void HookedWinFaultInjectHook(const char *name, void *var_ptr) {
  if (strcmp(name, "inject_io_error") == 0)
    *(int *)var_ptr = -5;
  else
    TrueWinFaultInjectHook(name, var_ptr);
}
#endif

int main(void) {
#if defined(_WIN32) || defined(_WIN64)
  mprobe_etw_register();
#endif

  /* Штатное поведение без инъекции. */
  CHECK(do_save("u1") == 0, "no-inject: save ok");

  /* Инъекция через tier=test fallback (без root, все платформы). */
  mprobe_fault_set_hook(test_inject_hook);
  CHECK(do_save("u2") == -5, "fault-hook inject: error observed");
  mprobe_fault_set_hook(NULL);
  CHECK(do_save("u3") == 0, "hook reset: save ok");

#if defined(_WIN32) && defined(MPROBE_USE_DETOURS)
  /* Windows: инъекция через Microsoft Detours на WinFaultInjectHook. */
  {
    LONG rc;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)TrueWinFaultInjectHook, HookedWinFaultInjectHook);
    rc = DetourTransactionCommit();
    CHECK(rc == NO_ERROR, "detours attach");
    CHECK(do_save("u4") == -5, "detours inject: error observed");
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID &)TrueWinFaultInjectHook, HookedWinFaultInjectHook);
    rc = DetourTransactionCommit();
    CHECK(rc == NO_ERROR, "detours detach");
    CHECK(do_save("u5") == 0, "detours reset: save ok");
  }
#endif

#if defined(_WIN32) || defined(_WIN64)
  mprobe_etw_unregister();
#endif

  if (g_failures) {
    printf("mprobe_test: %d FAILURE(S)\n", g_failures);
    return 1;
  }
  printf("mprobe_test: PASS\n");
  return 0;
}
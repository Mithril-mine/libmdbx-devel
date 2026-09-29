/* mprobe_providers.c — провайдеры и fallback-хук инъекции.
 * См. docs/engineering/testing-methodology.md, гл. 9-10. */
#include "mprobe.h"

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <TraceLoggingProvider.h>

/* Единственное определение ETW-провайдера (GUID уникален для libmdbx). */
TRACELOGGING_DEFINE_PROVIDER(
    mprobe_provider,
    "Libmdbx.MProbe.Provider",
    (0xf49b2c3a, 0x5d68, 0x4a91, 0x8e, 0x2c, 0x0d, 0x1f, 0x3b, 0x6a, 0x9c, 0x45));

void mprobe_etw_register(void) { TraceLoggingRegister(mprobe_provider); }
void mprobe_etw_unregister(void) { TraceLoggingUnregister(mprobe_provider); }

/* Заглушка для Microsoft Detours: в проде пустая (нулевой оверхед),
 * в тестах перехватывается по адресу. noinline обязателен. */
void __declspec(noinline) WinFaultInjectHook(const char *name, void *var_ptr) {
  (void)name;
  (void)var_ptr;
}
#endif /* _WIN32 */

/* ---------- tier=test fallback инъекции (без root, все платформы) ---------- */

static mprobe_fault_hook_fn g_mprobe_fault_impl = NULL;

void mprobe_fault_hook(const char *name, void *var_ptr) {
  if (g_mprobe_fault_impl != NULL)
    g_mprobe_fault_impl(name, var_ptr);
}

void mprobe_fault_set_hook(mprobe_fault_hook_fn fn) {
  g_mprobe_fault_impl = fn;
}
/*
 * mprobe.h — первая версия трёх макросов управляемого/контролируемого
 * тестирования (методика: docs/engineering/testing-methodology.md, гл. 9).
 *
 *   MPROBE_COLLECT(name, value) — метрики/статистика (канал измерения);
 *   MPROBE_WATCH(name, value)    — наблюдаемый факт/ветвь (канал событий);
 *   MPROBE_FAULT(name, var)      — инъекция: адрес var передаётся пробе,
 *                                  мутация выполняется трассировщиком или
 *                                  tier=test fallback'ом (mprobe_fault_hook,
 *                                  работает без root, на всех платформах).
 *
 * Механизмы (провайдер зафиксирован как "mprobe"):
 *   Linux   — LTTng-UST (ENABLE_LTTNG) для COLLECT/WATCH + USDT (sys/sdt.h,
 *             ENABLE_SYSTEMTAP) для FAULT;
 *   Windows — ETW TraceLogging для COLLECT/WATCH + Detours-заглушка
 *             WinFaultInjectHook для FAULT;
 *   macOS/BSD — DTrace (кодоген — отдельный шаг), FAULT через fallback-хук;
 *   прочее  — noop.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* tier=test fallback-хук инъекции: диспетчер к текущей реализации.
 * По умолчанию noop; тесты подменяют реализацию через mprobe_fault_set_hook().
 * Вызывается из MPROBE_FAULT на всех платформах. */
void mprobe_fault_hook(const char *name, void *var_ptr);
typedef void (*mprobe_fault_hook_fn)(const char *name, void *var_ptr);
void mprobe_fault_set_hook(mprobe_fault_hook_fn fn);

#if defined(_WIN32) || defined(_WIN64)
/* Заглушка для Microsoft Detours (noinline, нулевой оверхед в проде). */
void __declspec(noinline) WinFaultInjectHook(const char *name, void *var_ptr);
/* Регистрация ETW-провайдера (вызывается в main теста; можно и авто). */
void mprobe_etw_register(void);
void mprobe_etw_unregister(void);
#endif

#ifdef __cplusplus
}
#endif

/* =============================== Windows =============================== */
#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>
#include <TraceLoggingProvider.h>

#ifndef MPROBE_TRACELOGGING_DECLARED
#define MPROBE_TRACELOGGING_DECLARED
TRACELOGGING_DECLARE_PROVIDER(mprobe_provider);
#endif

#define MPROBE_COLLECT(name, value)                                                \
  TraceLoggingWrite(mprobe_provider, #name,                                        \
                    TraceLoggingIntPtr((INT_PTR)(uintptr_t)(value), "Value"))

#define MPROBE_WATCH(name, value)                                                  \
  TraceLoggingWrite(mprobe_provider, #name,                                        \
                    TraceLoggingIntPtr((INT_PTR)(uintptr_t)(value), "Value"))

#define MPROBE_FAULT(name, var)                                                    \
  do {                                                                             \
    TraceLoggingWrite(mprobe_provider, #name,                                      \
                      TraceLoggingIntPtr((INT_PTR)(uintptr_t)&(var), "VarAddress"));\
    WinFaultInjectHook(#name, &(var));                                             \
    mprobe_fault_hook(#name, &(var));                                              \
  } while (0)

/* =============================== Linux =============================== */
#elif defined(__linux__)

#ifdef ENABLE_LTTNG
#include "mprobe_lttng_provider.h"
#define MPROBE_COLLECT(name, value)                                                \
  tracepoint(mprobe_tp, collect, #name, (long)(value))
#define MPROBE_WATCH(name, value)                                                  \
  tracepoint(mprobe_tp, watch, #name, (long)(value))
#else
#define MPROBE_COLLECT(name, value) ((void)0)
#define MPROBE_WATCH(name, value) ((void)0)
#endif

#ifdef ENABLE_SYSTEMTAP
#include <sys/sdt.h>
#define MPROBE_FAULT(name, var)                                                    \
  do {                                                                             \
    STAP_PROBE1(mprobe, name, &(var));                                             \
    mprobe_fault_hook(#name, &(var));                                              \
  } while (0)
#else
#define MPROBE_FAULT(name, var) mprobe_fault_hook(#name, &(var))
#endif

/* ============================ macOS / BSD ============================ */
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||         \
    defined(__OpenBSD__)

#include <sys/sdt.h>
#define MPROBE_COLLECT(name, value) ((void)0) /* DTrace-кодоген — отдельный шаг */
#define MPROBE_WATCH(name, value) ((void)0)
#define MPROBE_FAULT(name, var) mprobe_fault_hook(#name, &(var))

/* ============================= Fallback ============================= */
#else
#define MPROBE_COLLECT(name, value) ((void)0)
#define MPROBE_WATCH(name, value) ((void)0)
#define MPROBE_FAULT(name, var) mprobe_fault_hook(#name, &(var))
#endif
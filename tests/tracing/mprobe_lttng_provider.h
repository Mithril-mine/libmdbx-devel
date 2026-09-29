/*
 * mprobe_lttng_provider.h — LTTng-UST tracepoint-провайдер (канал метрик).
 * Используется под -DENABLE_LTTNG на Linux. Определения событий:
 *   mprobe_tp:collect(name, value) — MPROBE_COLLECT;
 *   mprobe_tp:watch(name, value)    — MPROBE_WATCH.
 */
#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER mprobe_tp

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "mprobe_lttng_provider.h"

#if !defined(_MPROBE_LTTNG_PROVIDER_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _MPROBE_LTTNG_PROVIDER_H

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
    mprobe_tp,
    collect,
    TP_ARGS(const char *, name, long, value),
    TP_FIELDS(ctf_string(name, name) ctf_integer(long, value, value)))

TRACEPOINT_EVENT(
    mprobe_tp,
    watch,
    TP_ARGS(const char *, name, long, value),
    TP_FIELDS(ctf_string(name, name) ctf_integer(long, value, value)))

#endif /* _MPROBE_LTTNG_PROVIDER_H */

#include <lttng/tracepoint-event.h>
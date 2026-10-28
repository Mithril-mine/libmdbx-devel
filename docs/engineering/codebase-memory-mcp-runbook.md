# Runbook: codebase-memory-mcp (CBM) — диагностика и восстановление

> Dev-окружение: `codebase-memory-mcp` 0.12.1 (статичный бинарь в
> `~/.local/bin/codebase-memory-mcp`), MCP-конфиг для OpenCode в
> `~/.config/opencode/opencode.json`, модуль shared-graph-memory с мостом
> `cbm_*` (см. `skynet/MCP-shared-graph-memory/`).

## Модель работы (важно)

- **Демон живёт «по требованию»** и держится ровно пока существует
  присоединённый MCP-клиент (OpenCode-сессия, `codebase-memory-mcp cli`, ...
  ). При отсоединении **последнего** клиента демон завершается штатно:
  `daemon.runtime_stopping reason=last_committed_client_disconnected` /
  `daemon.lifetime_end reason=runtime_exited`.
- **Автономного долгоживущего демона без клиента нет** (нет systemd/cron/
  autostart). Поэтому «запустить демон вручную» для фоновой жизни без
  клиента — НЕ модель, не надо пытаться.
- `--ui=true` (HTTP-граф на 127.0.0.1:9749) тоже живёт только пока удержан
  клиентом MCP (проверка: удержать `codebase-memory-mcp --ui=true`-процесс
  с открытым stdin; порт слушает и отдаёт HTML).
- Соединение демону держит **каждый вызов**: `cli`, `hook-augment`, MCP-сервер.

## Симптомы поломки

| Симптом | Где | Причина |
| --- | --- | --- |
| `MCP error -32000: Connection closed, local mcp startup failed` | лог OpenCode `~/.local/share/opencode/log/*.log` | MCP-сервер (stdio) не смог поднять демон за таймаут (обычно 30с) |
| `CBM daemon could not start within 30000 ms` | stdout `codebase-memory-mcp` | то же |
| `CBM CLI could not start because a pre-coordination or unverified CBM generation is active; close all CBM sessions` | `codebase-memory-mcp cli ...` | stale-координация в `/tmp/cbm-daemon-<uid>/` от прошлой (умершей) генерации |
| `hook-augment: deadline_exceeded ms=2000` ×N | `~/.cache/codebase-memory-mcp/logs/hook-augment-timeouts.log` | плагин OpenCode `cbm-augment.ts` спавнит `hook-augment` на каждый инструмент, а демон недоступен |
| `cbm cli error: TimeoutExpired` / `cbm: unavailable` | shared-graph-memory `cbm_search` (store.py) | вызов CLI в подпроцессе с timeout=60с, демон не стартует |
| Демон постоянно умирает `reason=last_committed_client_disconnected` | `~/.cache/codebase-memory-mcp/logs/cbm-daemon.log` | штатно, если нет ни одного живого клиента |

## Диагностика

```bash
# 1. Живые процессы CBM
ps -u "$USER" -o pid,etime,cmd | rg -i 'codebase-memory|hook-augment'

# 2. Держатели координации / stale-артефакты
lsof /tmp/cbm-daemon-*/* 2>&1 | head
ls -la --time-style=full-iso /tmp/cbm-daemon-*/ 2>/dev/null | head

# 3. Жив ли слушатель сокета
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_UNIX)
try:
    s.connect(sorted(__import__('glob').glob('/tmp/cbm-daemon-*/*.sock'))[-1])
    print("listener OK")
except Exception as e:
    print("no listener:", e)
PY

# 4. Все ли lock'и свободны (flock)
for f in /tmp/cbm-daemon-*/*.lock; do flock -n "$f" -c 'echo free' 2>/dev/null || echo "BUSY: $f"; done

# 5. Лог демона (последняя активность, причина остановки)
tail -30 ~/.cache/codebase-memory-mcp/logs/cbm-daemon.log

# 6. Свежие записи демона в activity.db
python3 - <<'PY'
import sqlite3, os
f = os.path.expanduser('~/.cache/codebase-memory-mcp/activity.db')
con = sqlite3.connect(f'file:{f}?mode=ro', uri=True)
for r in con.execute('select id,ts,source,message from daemon_logs order by id desc limit 6'):
    print(r[0], r[1], r[2], '|', (r[3] or '')[:110])
PY
```

## Лечение (последовательность)

1. **Закрыть** временные CBM-клиенты (не критично: команды сами таймаутят).
2. **Удалить stale-координацию** в `/tmp/cbm-daemon-<uid>/` (даже если файлы
   «пустые»/lock'и свободны — их наличие блокирует старт новой генерации):
   ```bash
   rm -f /tmp/cbm-daemon-$UID/cbm-*.sock \
         /tmp/cbm-daemon-$UID/cbm-*.anc \
         /tmp/cbm-daemon-$UID/cbm-*.sock.identity \
         /tmp/cbm-daemon-$UID/cbm-*.lock \
         /tmp/cbm-daemon-$UID/cbm-*.rw \
         /tmp/cbm-daemon-$UID/cbm-*.turn \
         /tmp/cbm-daemon-$UID/cbm-version-cohort-*.lock
   rmdir  /tmp/cbm-daemon-$UID 2>/dev/null || true
   ```
   > НЕ трогать базы индексов/графа: `~/.cache/codebase-memory-mcp/*.db` —
   > это ценные данные. Удаляются только координационные артефакты.

3. **Проверить, что CLI больше не блокируется и демон поднимается:**
   ```bash
   codebase-memory-mcp config list            # раньше падало "pre-coordination active"
   # Проверка полного MCP-цикла (держим клиент ~8с): см. скрипт ниже
   ```
4. **Перезапустить OpenCode-сессию** (или дождаться новой): MCP-клиент
   стартует, поднимает и держит демон. Проверить инструменты
   `search_graph/trace_path/get_code_snippet/...`.
5. **Проверить shared-graph-memory**: `cbm_search` больше не должен
   таймаутить (быстрый ответ, не `unavailable`).

### Быстрая проверка MCP-цикла

```bash
python3 - <<'PY'
import subprocess, json, os, time
BIN = os.path.expanduser('~/.local/bin/codebase-memory-mcp')
p = subprocess.Popen([BIN], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
def send(obj):
    p.stdin.write(json.dumps(obj).encode()+b"\n"); p.stdin.flush()
send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})
send({"jsonrpc":"2.0","id":2,"method":"tools/list"})
time.sleep(5)
print("alive:", p.poll() is None)
print("out:", p.stdout.read1(512))
p.kill(); p.wait()
PY
```

## Профилактика при перезапуске среды

- **Перед стартом OpenCode** очистить stale-координацию (п.2 лечения) — тогда
  каждая новая сессия гарантированно поднимет демон.
- Простого «демона навсегда» нет — не пытаться поднять его вручную в фоне
  без клиента (процесс умирает с `last_committed_client_disconnected`).
- Если после очистки stale-файлы **появляются снова** — параллельная сессия
  только что спавнила CBM; повторить очистку, когда lsof/ps пусты.

## Связанные компоненты

- Плагин OpenCode: `~/.config/opencode/plugins/cbm-augment.ts` (спавнит
  `hook-augment` на каждый grep/glob/read/SessionStart; при недоступном
  демоне — `deadline_exceeded`, безвредные, но шумные).
- Мост shared-graph-memory: `skynet/MCP-shared-graph-memory/mcp/store.py`
  (`cbm_search`, graceful-degrade: `{"unavailable": true}` при сбое CLI).
- Дизайн модуля памяти: `docs/engineering/mcp-memory-design.md`.

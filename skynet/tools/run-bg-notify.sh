#!/usr/bin/env bash
# run-bg-notify.sh — асинхронный запуск команды с уведомлением на почту (TASK-39).
# Детачит команду (setsid + &), вызывающий возвращается мгновенно; монитор
# run-bg-notify.py ждёт завершения и шлёт письмо в inbox координатора.
#
# Usage:
#   run-bg-notify.sh "COMMAND" [--log FILE] [--nice N] [--inbox ENTITY] [--job ID]
#     --log FILE  лог stdout+stderr (по умолчанию .skynet/tmp/run-bg/<job>.log)
#     --nice N    приоритет nice (по умолчанию 5, кайдзен; 0 = без nice)
#     --inbox     адресат письма (по умолчанию skynet_inbox_0.coordinator)
#     --job ID    идентификатор задания (по умолчанию авто: bg-<time>-<pid>)
#
# Deploy: каноническая копия в репозитории skynet/tools/; рабочая — .skynet/tools/
#   (синхронизируется через notify-ctl.sh deploy [REPO_TOOLS]).
#
# Пример:
#   tools/run-bg-notify.sh "ctest -L 'ut\.dbi'" --nice 5
BASE=${SKYNET_ROOT:-/sourcecraft/workspace/.skynet}
TOOL=$BASE/tools/run-bg-notify.py

CMD=""
LOG=""
NICE=5
INBOX=""
JOB=""

while [ $# -gt 0 ]; do
  case "$1" in
    --log)   LOG="$2";   shift 2 ;;
    --nice)  NICE="$2";  shift 2 ;;
    --inbox) INBOX="$2"; shift 2 ;;
    --job)   JOB="$2";   shift 2 ;;
    *) CMD="${CMD:+$CMD }$1"; shift ;;
  esac
done

[ -z "$CMD" ] && {
  echo "usage: run-bg-notify.sh \"COMMAND\" [--log FILE] [--nice N] [--inbox ENTITY] [--job ID]" >&2
  exit 2
}

if [ -z "$LOG" ]; then
  LOG="$BASE/tmp/run-bg/bg-$(date +%Y%m%d-%H%M%S)-$$.log"
fi
mkdir -p "$(dirname "$LOG")"

args=(--cmd "$CMD" --log "$LOG" --nice "$NICE")
[ -n "$INBOX" ] && args+=(--inbox "$INBOX")
[ -n "$JOB" ] && args+=(--job "$JOB")

setsid python3 "$TOOL" "${args[@]}" >>"$LOG" 2>&1 </dev/null &
echo "run-bg-notify started pid $! job=${JOB:-auto} log=$LOG"

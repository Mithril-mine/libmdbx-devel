#!/usr/bin/env bash
# notify-daemon control (pidfile-based, fast start/stop/restart).
# Канонический источник: репозиторий skynet/tools/notify-ctl.sh (TASK-38).
# Usage: notify-ctl.sh {start|stop|restart|status}
BASE=/sourcecraft/workspace/.skynet
PIDF=$BASE/notify.pid
LOG=$BASE/notify.log
TOOL=$BASE/tools/notify-daemon.py

is_running() {
  [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null
}

case "${1:-status}" in
  start)
    if is_running; then echo "notify-daemon already running pid $(cat "$PIDF")"; exit 0; fi
    setsid python3 "$TOOL" --interval 60 >>"$LOG" 2>&1 </dev/null &
    echo $! > "$PIDF"
    sleep 1
    if kill -0 "$(cat "$PIDF")" 2>/dev/null; then
      echo "notify-daemon started pid $(cat "$PIDF")"
    else
      echo "notify-daemon FAILED to start — check $LOG" >&2; rm -f "$PIDF"; exit 1
    fi
    ;;
  stop)
    if is_running; then
      kill "$(cat "$PIDF")" && echo "notify-daemon stopped pid $(cat "$PIDF")"
      rm -f "$PIDF"
    else
      echo "notify-daemon not running (no live pidfile)"; rm -f "$PIDF"
    fi
    ;;
  restart)
    "$0" stop; sleep 1; "$0" start
    ;;
  status)
    if is_running; then echo "notify-daemon running pid $(cat "$PIDF")"; else echo "notify-daemon not running"; fi
    ;;
  *)
    echo "usage: $0 {start|stop|restart|status}"; exit 2
    ;;
esac
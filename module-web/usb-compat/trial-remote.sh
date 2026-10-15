#!/bin/sh
# Run only after authorizing this experimental kernel-module trial.
# No persistent startup registration or network/APN configuration writes.
set -eu
umask 077
ROOT=/tmp/qdc-usb-compat-trial
PARAM=/sys/module/qdc_usb_compat/parameters
if [ "${1:-}" = expire ]; then
  [ -n "${2:-}" ] || exit 2
  sleep 900
  # A simultaneous manual operation must not make the watchdog miss expiry.
  while :; do
    result=0
    sh "$ROOT/trial-remote.sh" stop "$2" || result=$?
    [ "$result" = 75 ] || exit "$result"
    sleep 1
  done
fi
if [ "${1:-}" = start ] || [ "${1:-}" = stop ]; then
  mkdir "$ROOT/control.lock" || { echo 'Trial control is busy'; exit 75; }
  loaded=0
  committed=0
  cleanup()
  {
    if [ "$loaded" = 1 ] && [ "$committed" = 0 ]; then
      printf 'N\n' > "$PARAM/enabled" 2>/dev/null || true
      rmmod qdc_usb_compat || true
      rm -f "$ROOT/lease"
    fi
    rmdir "$ROOT/control.lock" || true
  }
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM HUP
fi
case "${1:-}" in
  start)
    [ "$(uname -r)" = 3.18.44 ] || { echo 'Unsupported kernel'; exit 1; }
    [ -f "$ROOT/qdc_usb_compat.ko" ] || { echo 'Missing trial module'; exit 1; }
    [ ! -d "$PARAM" ] || { echo 'Existing trial must be stopped first'; exit 1; }
    insmod "$ROOT/qdc_usb_compat.ko" enabled=0
    loaded=1
    [ "$(cat "$PARAM/enabled")" = N ] || exit 1
    # Arrange rollback before changing forwarding packets. A power cycle
    # also clears this module because no boot-time loader is installed.
    lease=$(cat /proc/sys/kernel/random/uuid)
    [ -n "$lease" ] || exit 1
    printf '%s\n' "$lease" > "$ROOT/lease"
    nohup sh "$ROOT/trial-remote.sh" expire "$lease" >/dev/null 2>&1 &
    watchdog=$!
    printf '%s\n' "$watchdog" > "$ROOT/watchdog.pid"
    kill -0 "$watchdog"
    printf 'Y\n' > "$PARAM/enabled"
    [ "$(cat "$PARAM/enabled")" = Y ] || exit 1
    committed=1
    echo 'Trial enabled; automatic rollback in 15 minutes'
    ;;
  stop)
    # A previous watchdog must never disable a newer trial.
    if [ -n "${2:-}" ] && [ "$(cat "$ROOT/lease" 2>/dev/null || true)" != "$2" ]; then
      exit 0
    fi
    if [ -e "$PARAM/enabled" ]; then
      printf 'N\n' > "$PARAM/enabled"
      rmmod qdc_usb_compat
    fi
    rm -f "$ROOT/lease" "$ROOT/watchdog.pid"
    echo 'Trial disabled and module unloaded'
    ;;
  status)
    if [ -e "$PARAM/enabled" ]; then
      printf 'enabled='; cat "$PARAM/enabled"
      printf 'adjusted='; cat "$PARAM/adjusted"
    else
      echo 'enabled=N (module not loaded)'
    fi
    ;;
  *) echo 'usage: trial-remote.sh start|stop|status'; exit 2;;
esac

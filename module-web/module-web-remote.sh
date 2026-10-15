#!/bin/sh
# Runs inside the QDC507 module from /usrdata/qdc-web/control.sh.
# Only the explicitly selected install/start/stop actions write state.
set -eu
umask 077

ROOT=/usrdata/qdc-web
RUN=/run/qdc-web
WWW="$ROOT/www"
PIDFILE="$RUN/httpd.pid"
LOGFILE="$RUN/httpd.log"
AUDIOPID="$RUN/audio.pid"
AUDIOLOG="$RUN/audio.log"
ADDR=192.168.225.1
PORT=80

usage() {
    echo "usage: module-web-remote.sh {preflight|status|verify-stage|install-stage|start|stop|restore-last} [stage]" >&2
    exit 2
}

valid_stage() {
    case "${1:-}" in "$ROOT"/.stage-*) ;; *) return 1;; esac
    [ -d "$1" ] && [ -f "$1/index.html" ] && [ -f "$1/app.css" ] && [ -f "$1/app.js" ] && [ -f "$1/sms-codec.js" ] && [ -f "$1/communications.js" ] && [ -x "$1/cgi-bin/api" ] && [ -x "$1/cgi-bin/qdc-comm" ] && [ -x "$1/bin/qdc-audio" ] && [ -x "$1/bin/qdc-pcm" ] && [ -f "$1/audio.js" ] && [ -f "$1/call-audio.js" ] && [ -f "$1/pcm-worklet.js" ]
}

pid_start() {
    [ -r "/proc/$1/stat" ] || return 1
    awk '{print $22}' "/proc/$1/stat" 2>/dev/null
}

own_pid() {
    checkfile=${1:-$PIDFILE}
    [ -s "$checkfile" ] || return 1
    read -r pid started extra < "$checkfile" || return 1
    case "$pid:$started:$extra" in *[!0-9:]*|*::*) return 1;; esac
    [ -n "$pid" ] && [ -n "$started" ] || return 1
    current=$(pid_start "$pid") || return 1
    [ "$current" = "$started" ]
}

port_in_use() {
    awk 'NR > 1 && $2 ~ /:0050$/ && $4 == "0A" { found=1 } END { exit !found }' /proc/net/tcp /proc/net/tcp6 2>/dev/null
}

address_present() {
    ip -4 addr show 2>/dev/null | grep -Eq "[[:space:]]$ADDR/"
}

verify_runtime() {
    address_present || { echo "未发现模块地址 $ADDR" >&2; return 1; }
    [ -x /sbin/httpd ] || { echo '模块缺少 /sbin/httpd' >&2; return 1; }
    grep -q 'cgi-bin' /proc/mounts 2>/dev/null || true
}

stop_owned() {
    stop_audio
    if own_pid; then
        read -r pid started < "$PIDFILE"
        kill "$pid" 2>/dev/null || true
        count=0
        while own_pid; do
            count=$((count + 1))
            [ "$count" -lt 10 ] || { echo '网页服务未能在 10 秒内停止' >&2; return 1; }
            sleep 1
        done
    fi
    rm -f "$PIDFILE"
}

stop_audio() {
    if own_pid "$AUDIOPID"; then
        read -r pid started < "$AUDIOPID"
        kill "$pid" 2>/dev/null || true
        count=0
        while own_pid "$AUDIOPID"; do
            count=$((count + 1))
            [ "$count" -lt 15 ] || { echo '音频服务未正常退出，停止替换文件。' >&2; return 1; }
            sleep 1
        done
    fi
    rm -f "$AUDIOPID"
}
start_audio() {
    requested=${1:-}
    case "$requested" in https://*) ;; *) echo '必须指定 HTTPS 来源。' >&2; return 1;; esac
    host=${requested#https://}
    case "$host" in ''|*[!a-zA-Z0-9.:-]*|.*|*:*) echo '来源必须为 HTTPS 域名，不含路径或端口。' >&2; return 1;; esac
    [ -x "$WWW/bin/qdc-audio" ] && [ -x "$WWW/bin/qdc-pcm" ] || return 1
    verify_runtime
    stop_audio
    if awk 'NR>1 && $2 ~ /:2255$/ && $4=="0A" {found=1} END {exit !found}' /proc/net/tcp /proc/net/tcp6; then
        echo '8789 端口被占用，未启动。' >&2; return 1
    fi
    mkdir -p "$RUN/audio-tickets" "$RUN/sessions"
    chmod 700 "$RUN" "$RUN/audio-tickets" "$RUN/sessions"
    "$ROOT/standalone/qdc-config-store" set-origin "$requested" || return 1
    : > "$AUDIOLOG"; chmod 600 "$AUDIOLOG"
    nohup "$WWW/bin/qdc-audio" --serve </dev/null >"$AUDIOLOG" 2>&1 &
    pid=$!;started=$(pid_start "$pid") || return 1
    printf '%s %s\n' "$pid" "$started" > "$AUDIOPID";chmod 600 "$AUDIOPID"
    sleep 1
    own_pid "$AUDIOPID" || { echo '音频服务启动失败。' >&2; return 1; }
    echo '音频服务已启动；等待用户网页登录并接通电话，不会自行拨号。'
}
start_web() {
    verify_runtime
    if own_pid; then
        echo '网页服务已运行。'
        return 0
    fi
    if port_in_use; then
        echo "端口 $PORT 已被其他服务占用，未启动网页服务。" >&2
        return 1
    fi
    [ -x "$WWW/cgi-bin/api" ] || { echo '网页程序未安装。' >&2; return 1; }
    mkdir -p "$RUN/sessions"
    chmod 700 "$RUN" "$RUN/sessions"
    : > "$LOGFILE"
    chmod 600 "$LOGFILE"
    nohup /sbin/httpd -f -p "$ADDR:$PORT" -h "$WWW" </dev/null >"$LOGFILE" 2>&1 &
    pid=$!
    started=$(pid_start "$pid") || { echo 'httpd 未能启动。' >&2; return 1; }
    printf '%s %s\n' "$pid" "$started" > "$PIDFILE"
    chmod 600 "$PIDFILE"
    sleep 1
    own_pid && port_in_use || { echo 'httpd 启动后未监听端口。' >&2; return 1; }
}

restore_last() {
    if [ -s "$RUN/last-backup" ]; then
        read -r backup < "$RUN/last-backup"
        case "$backup" in "$ROOT"/www.backup.*) ;; *) echo '备份路径无效。' >&2; return 1;; esac
        [ -d "$backup" ] || { echo '指定备份目录已不存在。' >&2; return 1; }
    else
        backup=$(ls -dt "$ROOT"/www.backup.* 2>/dev/null | head -1 || true)
    fi
    [ -n "$backup" ] || { echo '没有可恢复的网页备份。' >&2; return 1; }
    stop_owned
    stamp=$(date +%s 2>/dev/null || echo rollback)
    if [ -d "$WWW" ]; then mv "$WWW" "$ROOT/www.failed.$stamp"; fi
    mv "$backup" "$WWW"
    rm -f "$RUN/last-backup"
    echo "已恢复网页目录：$backup"
}

preflight() {
    echo '== 模块 =='
    uname -a
    echo '== 地址 =='
    ip -4 addr show 2>/dev/null | grep -E "inet .*($ADDR|192\.168\.225\.)" || true
    echo '== httpd =='
    /sbin/httpd --help 2>&1 | head -30 || true
    echo '== 空间 =='
    df -h /usrdata /run 2>&1 || true
    echo '== 端口 80 =='
    if port_in_use; then echo '已被占用'; else echo '空闲'; fi
    echo '== 本工具服务 =='
    if own_pid; then echo '运行中'; else echo '未运行'; fi
    echo '== 现有文件 =='
    ls -ld "$ROOT" "$WWW" "$ROOT/private" "$RUN" 2>/dev/null || true
}

status() {
    echo '== service =='
    if own_pid; then
        read -r pid started < "$PIDFILE"
        echo "running pid=$pid start=$started"
    else
        echo 'not-running'
    fi
    echo '== listener =='
    if port_in_use; then echo 'port-80-in-use'; else echo 'port-80-free'; fi
    echo '== files =='
    ls -l "$WWW" "$WWW/cgi-bin/api" "$ROOT/private/credential" 2>/dev/null || true
    echo '== recent log =='
    tail -n 20 "$LOGFILE" 2>/dev/null || true
}

install_stage() {
    stage=${1:-}
    valid_stage "$stage" || { echo '网页暂存包不完整或路径不安全。' >&2; return 1; }
    verify_runtime
    if port_in_use && ! own_pid; then
        echo "端口 $PORT 已被其他服务占用，拒绝替换网页目录。" >&2
        return 1
    fi
    if [ -x "$WWW/cgi-bin/qdc-comm" ]; then
        calls=$("$WWW/cgi-bin/qdc-comm" call-list)
        case "$calls" in *'"ok":true'*'"calls":[]'*) ;; *) echo '电话未空闲或无法确认，未替换网页。' >&2; return 1;; esac
    fi
    mkdir -p "$ROOT/private" "$RUN/sessions"
    chmod 700 "$ROOT" "$ROOT/private" "$RUN" "$RUN/sessions"
    stop_owned
    stamp="$(date +%s 2>/dev/null || echo install).$$"
    if [ -d "$WWW" ]; then
        mv "$WWW" "$ROOT/www.backup.$stamp"
        printf '%s\n' "$ROOT/www.backup.$stamp" > "$RUN/last-backup"
        chmod 600 "$RUN/last-backup"
    fi
    mv "$stage" "$WWW"
    chmod 700 "$WWW/bin" "$WWW/bin/qdc-audio" "$WWW/bin/qdc-pcm"
    chmod 755 "$WWW/cgi-bin/api" "$WWW/cgi-bin/qdc-comm"
    chmod 644 "$WWW"/*.html "$WWW"/*.css "$WWW"/*.js "$WWW"/*.gz
    echo '网页目录已替换；尚未启动服务。'
}

case "${1:-}" in
    preflight) preflight ;;
    status) status ;;
    verify-stage) valid_stage "${2:-}" ;;
    install-stage) install_stage "${2:-}" ;;
    audio-start) start_audio "${2:-}" ;;
    audio-stop) stop_audio ;;
    start) start_web ;;
    stop) stop_owned ;;
    restore-last) restore_last ;;
    *) usage ;;
esac

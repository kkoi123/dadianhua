#!/bin/sh
# Execute inside module after specific authorization for the new boot hook and
# runtime voice/data initialization. Adds only two owned rootfs entries.
set -eu
umask 077
ROOT=/usrdata/qdc-web
APP=$ROOT/standalone
INIT=/etc/init.d/qdc-web-standalone
LINK=/etc/rc5.d/S99qdc-web-standalone
restore_ro=0
finish() {
    result=$?
    trap - EXIT
    if [ "$restore_ro" -eq 1 ]; then
        sync
        mount -o remount,ro / || { echo '无法恢复系统分区只读，请检查。' >&2; exit 1; }
    fi
    exit "$result"
}
trap finish EXIT
root_writable() {
    options=$(awk '$2=="/" && $1!="rootfs" {print $4}' /proc/mounts)
    case ",$options," in *,ro,*) mount -o remount,rw /; restore_ro=1 ;;
        *,rw,*) ;; *) echo '未知根分区挂载状态，未修改。' >&2; exit 1;; esac
}
owned_init() {
    [ -f "$INIT" ] && cmp -s "$INIT" "$APP/init.sh"
}
idle() {
    calls=$("$ROOT/www/cgi-bin/qdc-comm" call-list)
    case "$calls" in *'"ok":true'*'"calls":[]'*) ;; *) echo '电话未空闲或无法确认，未启用。' >&2; exit 1;; esac
}
case "${1:-}" in
    status)
        if [ -f "$ROOT/private/standalone-enabled" ]; then echo '独立启动已启用'; else echo '独立启动未启用'; fi
        if owned_init && [ "$(readlink "$LINK" 2>/dev/null)" = ../init.d/qdc-web-standalone ]; then echo '开机入口已安装'; else echo '开机入口未安装或不属于本程序'; fi
        exit 0 ;;
    enable)
        [ "${2:-}" = --apply ] || { echo '需要 --apply 和用户对开机修改的授权。' >&2; exit 2; }
        sh "$APP/service.sh" check
        idle
        if [ -e "$INIT" ] || [ -L "$INIT" ]; then owned_init || { echo '已有其他开机文件，未覆盖。' >&2; exit 1; }; fi
        if [ -e "$LINK" ] || [ -L "$LINK" ]; then
            [ "$(readlink "$LINK")" = ../init.d/qdc-web-standalone ] || exit 1
        fi
        root_writable
        if [ ! -e "$INIT" ]; then
            cp "$APP/init.sh" "$INIT.qdc-new"
            chmod 755 "$INIT.qdc-new"
            mv "$INIT.qdc-new" "$INIT"
        fi
        [ -L "$LINK" ] || ln -s ../init.d/qdc-web-standalone "$LINK"
        # Complete rootfs writes and restore ro before launching applications.
        sync
        if [ "$restore_ro" -eq 1 ]; then mount -o remount,ro /; restore_ro=0; fi
        printf 'standalone-v1\n' > "$ROOT/private/standalone-enabled"
        sh "$APP/service.sh" start
        echo '独立开机服务已启用；尚需充电宝冷启动和真实通话验证。' ;;
    disable)
        [ "${2:-}" = --apply ] || exit 2
        [ ! -e "$INIT" ] || owned_init || { echo '开机文件已被其他程序修改，未删除。' >&2; exit 1; }
        [ ! -L "$LINK" ] || [ "$(readlink "$LINK")" = ../init.d/qdc-web-standalone ] || exit 1
        rm -f "$ROOT/private/standalone-enabled"
        sh "$APP/service.sh" stop
        root_writable
        [ ! -L "$LINK" ] || rm "$LINK"
        [ ! -f "$INIT" ] || rm "$INIT"
        echo '仅移除了本程序开机入口和穿透进程；网页、当前通话驱动及数据连接保留。' ;;
    *) echo 'usage: boot-control.sh {status|enable --apply|disable --apply}' >&2; exit 2 ;;
esac

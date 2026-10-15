#!/bin/sh
# Fresh installs only. This script is distributed, not run by the packager.
set -eu
umask 077
ROOT=/usrdata/qdc-web
RUN=/run/qdc-web
APP=$ROOT/standalone

fail() { echo "$*" >&2; exit 1; }
preflight() {
    [ "$(id -u)" = 0 ] || fail '需要模块的 root ADB 权限。'
    [ "$(uname -m)" = armv7l ] && [ "$(uname -r)" = 3.18.44 ] || fail '仅支持 ARMv7 / Linux 3.18.44 的 QDC507 原厂系统。'
    for file in /sbin/httpd /lib/ld-linux.so.3 /usr/lib/libqmi_cci.so.1 /usr/lib/libqmiservices.so.1 /usr/lib/libql_lib_audio.so.1 /usr/bin/curl /usr/bin/alsaucm_test; do
        [ -f "$file" ] || fail "缺少原厂依赖：$file"
    done
    for cmd in sha256sum setsid flock ip awk readlink timeout find nohup sync; do
        command -v "$cmd" >/dev/null || fail "缺少系统工具：$cmd"
    done
    [ -d /usrdata ] && [ -d /run ] || fail '缺少数据或运行分区。'
}
fresh_check() {
    preflight
    [ ! -e "$ROOT" ] && [ ! -L "$ROOT" ] || fail '模块已有项目，首次安装包拒绝覆盖，密码及记录未修改。'
    [ ! -e /etc/init.d/qdc-web-standalone ] && [ ! -L /etc/rc5.d/S99qdc-web-standalone ] || fail '模块已有本项目开机入口，请先核对旧安装。'
    [ ! -e "$RUN" ] && [ ! -L "$RUN" ] || fail '模块存在本项目运行目录，请先核对旧安装。'
    ip -4 addr show | grep -Eq '[[:space:]]192\.168\.225\.1/' || fail '未发现模块 USB 管理地址 192.168.225.1。'
    # Refuse every required listener, including the independent Oray RPC.
    if awk 'NR>1 && $4=="0A" && $2 ~ /:(0050|2254|2255|3EBF)$/ {found=1} END {exit !found}' /proc/net/tcp /proc/net/tcp6; then
        fail '80、8788、8789 或 16063 端口已占用，未替换已有服务。'
    fi
    free_kb=$(df -Pk /usrdata | awk 'END {print $4}')
    case "$free_kb" in ''|*[!0-9]*) fail '无法读取数据分区空间。';; esac
    [ "$free_kb" -ge 6144 ] || fail '数据分区至少需要 6 MiB 可用空间。'
    echo '首次安装预检通过。'
}
require_apply() { [ "${1:-}" = --apply ] || fail '写入操作需要显式 --apply。'; }
valid_origin() {
    case "$1" in https://*) ;; *) fail '请输入 HTTPS 公网域名。';; esac
    host=${1#https://}
    case "$host" in ''|.*|*[!a-zA-Z0-9.-]*) fail '域名不得包含路径、端口或特殊字符。';; esac
    [ "${#host}" -le 253 ] || fail '域名过长。'
}
idle() {
    calls=$("$ROOT/www/cgi-bin/qdc-comm" call-list) || fail '无法确认电话空闲。'
    case "$calls" in *'"ok":true'*'"calls":[]'*) ;; *) fail '电话未空闲，未修改服务。';; esac
}

case "${1:-}" in
    check) fresh_check ;;
    install)
        require_apply "${2:-}"
        domain=${3:-https://example.invalid}; valid_origin "$domain"
        fresh_check
        PACKAGE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
        case "$PACKAGE" in /tmp/qdc-release-*) ;; *) fail '安装文件必须位于安装工具生成的临时目录。';; esac
        token=${PACKAGE#/tmp/qdc-release-}
        case "$token" in ''|*[!a-f0-9]*) fail '临时目录名称无效。';; esac
        [ "${#token}" = 32 ] || fail '临时目录名称无效。'
        (cd "$PACKAGE/payload"; sha256sum -c PAYLOAD_SHA256 >/dev/null) || fail '安装文件校验失败。'
        NEW=/usrdata/.qdc-web-release-$token
        INSTALL_LOCK=/run/qdc-web-install-lock
        mkdir "$INSTALL_LOCK" || fail '已有首次安装任务，停止并行安装。'
        candidate_created=0
        # Only the unpublished candidate may be removed on pre-commit failure.
        trap 'if [ "$candidate_created" = 1 ] && [ -d "$NEW" ]; then rm -rf "$NEW"; fi; rmdir "$INSTALL_LOCK"' EXIT
        mkdir "$NEW"
        candidate_created=1
        cp -R "$PACKAGE/payload/." "$NEW/"
        find "$NEW" -type d -exec chmod 700 '{}' ';'
        find "$NEW" -type f -exec chmod 600 '{}' ';'
        chmod 700 "$NEW/control.sh" "$NEW/www/cgi-bin/"* "$NEW/www/bin/"* \
            "$NEW/standalone/"*.sh "$NEW/standalone/qdc-entry" \
            "$NEW/standalone/qdc-network-watch" "$NEW/standalone/qdc-config-store" \
            "$NEW/standalone/phtunnel" "$NEW/standalone/lib/libc.so"
        ln -s standalone/lib/libc.so "$NEW/ld"
        ln -s ../phtunnel "$NEW/standalone/lib/phtunnel"
        mkdir "$NEW/private"
        printf '0\n' > "$NEW/private/tunnel-policy"
        # Preserve the stock cellular and USB policies. Compatibility is opt-in.
        printf '0\n' > "$NEW/private/usb-compat-policy"
        QDC_WEB_ROOT="$NEW" "$NEW/www/cgi-bin/api" --init </dev/null > "$NEW/private/initial-password.txt"
        [ -s "$NEW/private/credential" ] && [ -s "$NEW/private/initial-password.txt" ] || fail '管理员密码生成失败。'
        # Root does not exist; no old private directory is copied or replaced.
        [ ! -e "$ROOT" ] && [ ! -L "$ROOT" ] || fail '安装期间发现已有项目，停止提交。'
        mv "$NEW" "$ROOT"
        mkdir "$RUN"; chmod 700 "$RUN"
        "$APP/qdc-config-store" set-origin "$domain"
        sync
        sh "$APP/service.sh" check
        # Local UI and events first. Audio/driver startup is a separate action.
        sh "$ROOT/control.sh" start
        sh "$APP/service.sh" entry-start
        sh "$APP/service.sh" events-start
        rmdir "$INSTALL_LOCK"
        trap - EXIT
        echo '首次安装完成。管理员密码由电脑安装工具单独保存。'
        ;;
    start)
        require_apply "${2:-}"; preflight; idle
        sh "$APP/service.sh" once
        echo '服务启动已执行，请在网页核对网络及音频状态。'
        ;;
    set-domain)
        require_apply "${2:-}"; preflight; valid_origin "${3:-}"; idle
        # Stop only our services so the entry reloads its allowed origin.
        sh "$APP/service.sh" stop
        sh "$ROOT/control.sh" stop
        "$APP/qdc-config-store" set-origin "$3"
        sh "$APP/service.sh" once
        if [ -f "$ROOT/private/standalone-enabled" ]; then sh "$APP/service.sh" start; fi
        echo '公网来源域名已保存，请另在花生壳后台配置映射。'
        ;;
    enable-startup)
        require_apply "${2:-}"; preflight
        sh "$APP/boot-control.sh" enable --apply
        ;;
    init-oray)
        require_apply "${2:-}"; preflight; idle
        sh "$APP/service.sh" check
        [ ! -e "$ROOT/private/module-oray.json" ] && [ ! -e "$ROOT/private/module-oray.json.last-good" ] || fail '已有花生壳身份，拒绝重新注册；请直接到网页扫码。'
        [ ! -e "$RUN/module-oray.json" ] && [ ! -e "$RUN/module-oray.pid" ] || fail '存在花生壳运行记录，未启动第二个客户端。'
        [ "$(sh "$APP/service.sh" tunnel-status)" = stopped ] || fail '花生壳服务未停止。'
        if awk 'NR>1 && $4=="0A" && $2 ~ /:3EBF$/ {found=1} END {exit !found}' /proc/net/tcp /proc/net/tcp6; then
            fail '花生壳 RPC 端口已占用。'
        fi
        [ ! -e "$RUN/standalone-maintenance" ] || fail '已有维护任务，未注册新设备。'
        exec 9>"$RUN/data.lock"
        flock -n 9 || fail '已有联网设置任务，未注册新设备。'
        : > "$RUN/standalone-maintenance"
        finish_registration() {
            result=$?; trap - EXIT
            sh "$APP/service.sh" tunnel-stop >/dev/null 2>&1 || true
            rm -f "$RUN/module-oray-import.json" "$RUN/standalone-maintenance"
            exit "$result"
        }
        trap finish_registration EXIT
        # The SDK registers its own new identity; never reuse another device SN.
        printf '0\n' > "$ROOT/private/tunnel-policy"
        (cd "$APP"; LD_LIBRARY_PATH="$APP/lib" exec nohup "$APP/phtunnel" \
            --rpc --rpcport 16063 --config "$RUN/module-oray.json" \
            --log /dev/null --pid "$RUN/module-oray.pid" --forward 127.0.0.1:8788 \
            </dev/null >/dev/null 2>&1 9>&-) &
        sdk_pid=$!
        sdk_start=$(awk '{print $22}' /proc/"$sdk_pid"/stat)
        printf '%s %s\n' "$sdk_pid" "$sdk_start" > "$RUN/standalone-tunnel.pid"
        registered=0
        for attempt in 1 2 3 4 5 6 7 8 9 10 11 12; do
            sleep 5
            # Import accepts only complete JSON with SDK-generated SN/auth;
            # incomplete snapshots are rejected without overwriting saved state.
            if [ -s "$RUN/module-oray.json" ]; then
                cp "$RUN/module-oray.json" "$RUN/module-oray-import.json"
                chmod 600 "$RUN/module-oray-import.json"
                if "$APP/qdc-config-store" import >/dev/null 2>&1; then registered=1; break; fi
            fi
        done
        [ "$registered" = 1 ] || fail '服务器尚未完成新设备注册，身份未保存。检查网络后重试前，先人工核对运行文件，勿重复注册。'
        sh "$APP/service.sh" tunnel-stop >/dev/null 2>&1
        rm -f "$RUN/module-oray-import.json"
        printf '1\n' > "$ROOT/private/tunnel-policy"; sync
        sh "$APP/service.sh" tunnel-apply 9>&-
        rm -f "$RUN/standalone-maintenance"
        trap - EXIT
        echo '新设备身份已保存，请到应用 → 花生壳生成二维码并绑定自己的账号。'
        ;;
    *) fail '操作：check、install、start、set-domain、enable-startup、init-oray。' ;;
esac

#!/bin/sh
# Module service. Persistent files stay in our own /usrdata directory;
# drivers, calibration, data connection and fallback address are runtime state.
set -eu
umask 077
ROOT=/usrdata/qdc-web
RUN=/run/qdc-web
APP=$ROOT/standalone
mkdir -p "$RUN"
chmod 700 "$RUN"
alive() {
    pid_record=$1
    [ -s "$pid_record" ] || return 1
    read -r task_pid task_start extra < "$pid_record" || return 1
    case "$task_pid:$task_start:$extra" in *[!0-9:]*|*::*) return 1;; esac
    [ -n "$task_pid" ] && [ -n "$task_start" ] || return 1
    task_state=$(awk '{print $3}' /proc/"$task_pid"/stat 2>/dev/null) || return 1
    case "$task_state" in Z|X|'') return 1;; esac
    [ "$(awk '{print $22}' /proc/"$task_pid"/stat 2>/dev/null)" = "$task_start" ]
}
remember() {
    task_pid=$1; file=$2
    task_start=$(awk '{print $22}' /proc/"$task_pid"/stat)
    printf '%s %s\n' "$task_pid" "$task_start" > "$file"
}
origin() {
    read -r public_origin < "$ROOT/private/audio-origin"
    case "$public_origin" in https://*) ;; *) return 1;; esac
    host=${public_origin#https://}
    case "$host" in ''|*[!a-zA-Z0-9.-]*|.*) return 1;; esac
}
verify() {
    [ "$(uname -m)" = armv7l ] && [ "$(uname -r)" = 3.18.44 ]
    [ -x "$ROOT/control.sh" ] && [ -f "$ROOT/private/credential" ]
    command -v setsid >/dev/null
    [ "$(readlink "$ROOT/ld")" = standalone/lib/libc.so ]
    [ "$(readlink "$APP/lib/phtunnel")" = ../phtunnel ]
    (cd "$APP" && sha256sum -c sha256sums >/dev/null)
    "$APP/qdc-config-store" recover
    origin
}
address() {
    # USB tethering need not be present on a power bank. Keep the existing
    # web/audio bind address locally available without changing USB settings.
    if ! ip -4 addr show | grep -Eq '[[:space:]]192\.168\.225\.1/'; then
        ip addr add 192.168.225.1/32 dev lo
        printf 'created\n' > "$RUN/standalone-local-address"
    fi
}
data_connection() {
    # Data policy controls both IP families; local USB routes remain intact.
    # Restore only stock daemons if absent. Their normal cold-boot startup
    # is unchanged. An orphaned rmnet route alone does not prove a live link.
    if ! pidof netmgrd >/dev/null 2>&1; then
        sh /etc/init.d/netmgrd start || return 1
    fi
    if ! pidof ql_manager_server >/dev/null 2>&1; then
        # The stock manager refuses startup when its PID file remains after
        # an OOM kill. Remove only a numeric record whose process is absent.
        if [ -f /run/ql_manager_server.pid ]; then
            read -r manager_pid < /run/ql_manager_server.pid || return 1
            case "$manager_pid" in ''|*[!0-9]*) return 1;; esac
            [ ! -d "/proc/$manager_pid" ] || return 1
            rm /run/ql_manager_server.pid || return 1
        fi
        # A vendor child (radish) can inherit the manager's flock. After the
        # manager dies that child retains the old inode lock. Only detach a
        # lock recorded by the kernel for a PID that no longer exists.
        manager_lock=/tmp/.ql_manager_server.lock
        if [ -f "$manager_lock" ] && [ ! -L "$manager_lock" ]; then
            lock_inode=$(stat -c %i "$manager_lock") || return 1
            lock_owner=$(awk -v inode="$lock_inode" \
                '$2=="FLOCK" && $4=="WRITE" {split($6,key,":"); if(key[3]==inode) print $5}' /proc/locks)
            if [ -n "$lock_owner" ]; then
                case "$lock_owner" in *[!0-9]*) return 1;; esac
                [ ! -d "/proc/$lock_owner" ] || return 1
                rm "$manager_lock" || return 1
            fi
        fi
        nohup sh /etc/init.d/start_ql_manager_server_le start \
            </dev/null > "$RUN/stock-manager-start.log" 2>&1 || return 1
        manager_wait=0
        while ! pidof ql_manager_server >/dev/null 2>&1; do
            manager_wait=$((manager_wait+1))
            [ "$manager_wait" -lt 20 ] || return 1
            sleep 0.1
        done
    fi
    # The application serializes user switches and reconnects. A saved OFF
    # policy never falls back to the old unconditional data_call up command.
    "$ROOT/www/cgi-bin/api" --data-reconcile
}
voice() {
    if grep -q '^qdc507_afe ' /proc/modules; then
        echo '旧语音驱动仍存在，保留网页，不热替换驱动。' >&2; return 1
    fi
    (cd "$APP/voice" && sha256sum -c sha256sums >/dev/null) || return 1
    for module in qdc507_aprv3 qdc507_voice; do
        grep -q "^$module " /proc/modules || insmod "$APP/voice/$module.ko" || return 1
    done
    mkdir -p /dev/snd || return 1
    for name in controlC0 pcmC0D4p pcmC0D4c pcmC0D5p pcmC0D6c; do
        [ -r "/sys/class/sound/$name/dev" ] || return 1
        if [ ! -c "/dev/snd/$name" ]; then
            read -r device < "/sys/class/sound/$name/dev" || return 1
            case "$device" in *[!0-9:]*|:*|*:) return 1;; esac
            mknod "/dev/snd/$name" c "${device%%:*}" "${device#*:}" || return 1
            chmod 660 "/dev/snd/$name" || return 1
        fi
    done
    grep -q 'mdm9607-tomtom-i2s' /proc/asound/cards || return 1
    if alive /run/mavo-alsaucm.pid && [ -p /run/alsaucm_test ] &&
        [ "$(tr '\000' '\n' < /proc/"$task_pid"/cmdline | head -1)" = /usr/bin/alsaucm_test ] &&
        grep -q 'ACDB -> Sent VocProc Cal!' /run/mavo-alsaucm.log; then return 0; fi
    # Refuse to disturb any calibration process owned by another application.
    for path in /proc/[0-9]*/cmdline; do
        [ "$(tr '\000' '\n' < "$path" 2>/dev/null | head -1)" != /usr/bin/alsaucm_test ] || {
            echo '已有未确认归属的校准服务，未重置。' >&2; return 1;
        }
    done
    rm -f /run/alsaucm_test
    nohup /usr/bin/alsaucm_test </dev/null >/run/mavo-alsaucm.log 2>&1 &
    remember "$!" /run/mavo-alsaucm.pid || return 1
    count=0
    while [ ! -p /run/alsaucm_test ]; do
        count=$((count+1)); [ "$count" -lt 50 ] || return 1
        alive /run/mavo-alsaucm.pid || return 1
        sleep 0.1
    done
    # A FIFO write must not stall service startup if calibration crashes.
    timeout -t 5 sh -c "printf 'open snd_soc_msm_9x07_Tomtom_I2S\\nset _verb VoLTE\\nset _enadev Auxpcm Rx\\nset _enadev Auxpcm Tx\\n' > /run/alsaucm_test" || return 1
    count=0
    while ! grep -q 'ACDB -> Sent VocProc Cal!' /run/mavo-alsaucm.log; do
        count=$((count+1)); [ "$count" -lt 100 ] || return 1
        alive /run/mavo-alsaucm.pid || return 1
        sleep 0.1
    done
}
entry() {
    if alive "$RUN/standalone-entry.pid"; then
        cmp -s /proc/"$task_pid"/exe "$APP/qdc-entry" && return 0
        kill "$task_pid"
        count=0
        while alive "$RUN/standalone-entry.pid"; do
            count=$((count+1)); [ "$count" -lt 30 ] || return 1
            sleep 0.1
        done
    fi
    origin
    nohup "$APP/qdc-entry" "$public_origin" </dev/null > "$RUN/standalone-entry.log" 2>&1 &
    remember "$!" "$RUN/standalone-entry.pid"
    sleep 1; alive "$RUN/standalone-entry.pid"
}
tunnel_task() {
    case "$1" in ''|*[!0-9]*) return 1;; esac
    executable=$(readlink /proc/"$1"/exe 2>/dev/null) || return 1
    case "$executable" in "$APP/phtunnel"|"$APP/lib/libc.so") ;; *) return 1;; esac
    tr '\000' '\n' < /proc/"$1"/cmdline | grep -Fxq -e "$ROOT/private/module-oray.json" -e "$RUN/module-oray.json"
}
tunnel_tasks() {
    for process in /proc/[0-9]*; do
        candidate=${process#/proc/}
        if tunnel_task "$candidate"; then printf '%s\n' "$candidate"; fi
    done
}
stop_tunnel() {
    # Freeze the verified owned family before killing it: no watchdog respawn.
    for pass in 1 2; do
        tasks=$(tunnel_tasks)
        for candidate in $tasks; do tunnel_task "$candidate" && kill -STOP "$candidate" || true; done
        if [ -n "$tasks" ] && [ -x "$APP/qdc-config-store" ]; then
            "$APP/qdc-config-store" checkpoint-stopped || true
        fi
        for candidate in $tasks; do tunnel_task "$candidate" && kill -KILL "$candidate" || true; done
    done
    rm -f "$RUN/standalone-tunnel.pid" "$RUN/module-oray-launch.pid" "$RUN/module-oray.pid"
}
tunnel_policy() {
    tunnel_enabled=1
    if [ -e "$ROOT/private/tunnel-policy" ]; then
        tunnel_enabled=$(cat "$ROOT/private/tunnel-policy") || return 1
        case "$tunnel_enabled" in 0|1) ;; *) return 1;; esac
    fi
}
tunnel_pending() {
    [ -s "$RUN/tunnel-job" ] || return 1
    read -r tunnel_started extra < "$RUN/tunnel-job" || return 1
    case "$tunnel_started:$extra" in *[!0-9:]*|:*) return 1;; esac
    [ -z "$extra" ] || return 1
    tunnel_now=$(cut -d . -f 1 /proc/uptime)
    [ "$tunnel_now" -ge "$tunnel_started" ] && [ $((tunnel_now-tunnel_started)) -lt 60 ]
}
tunnel_status() {
    # Report the owned client, not a fabricated cloud connection status.
    tasks=$(tunnel_tasks)
    if [ -n "$tasks" ]; then echo running
    elif [ -f "$RUN/standalone-tunnel-blocked" ]; then echo blocked
    else echo stopped; fi
}
tunnel() {
    # Do not disconnect a public request before its delayed worker runs.
    if [ "${1:-0}" != 1 ] && tunnel_pending; then return 0; fi
    tunnel_policy || { echo '花生壳开关配置无效，未启动服务。' >&2; return 1; }
    if [ "$tunnel_enabled" = 0 ]; then
        current_calls=$("$ROOT/www/cgi-bin/qdc-comm" call-list) || return 1
        case "$current_calls" in *'"ok":true'*'"calls":[]'*) stop_tunnel;; *) return 1;; esac
        return 0
    fi
    [ ! -f "$RUN/standalone-tunnel-blocked" ] || { echo '客户端异常后已停止自动重试，请检查。' >&2; return 1; }
    tasks=$(tunnel_tasks)
    set -- $tasks
    if [ "$#" -gt 4 ]; then
        stop_tunnel
        printf 'excess-owned-processes\n' > "$RUN/standalone-tunnel-blocked"
        echo '客户端进程数量异常，已停止本程序客户端，避免资源耗尽。' >&2
        return 1
    fi
    if alive "$RUN/standalone-tunnel.pid" && tunnel_task "$task_pid"; then return 0; fi
    # Official pid file includes pid=, SN and tokens. Parse only the number;
    # never print the file or confuse it with our pid/start-time records.
    if [ -s "$RUN/module-oray.pid" ]; then
        candidate=$(sed -n 's/^pid=\([0-9][0-9]*\)$/\1/p' "$RUN/module-oray.pid" | head -1)
        if tunnel_task "$candidate"; then remember "$candidate" "$RUN/standalone-tunnel.pid"; return 0; fi
    fi
    if [ "$#" -gt 0 ]; then remember "$1" "$RUN/standalone-tunnel.pid"; return 0; fi
    # Adopt the already verified check process only when its start and argv
    # match this installation. Do not run two clients with the same identity.
    if [ -s "$RUN/module-oray-launch.pid" ]; then
        read -r candidate < "$RUN/module-oray-launch.pid"
        case "$candidate" in ''|*[!0-9]*) return 1;; esac
        if tunnel_task "$candidate"; then
            remember "$candidate" "$RUN/standalone-tunnel.pid"; return 0
        fi
    fi
    # The SDK builds its child path from /proc/self/exe's directory plus
    # "phtunnel". A private lib/phtunnel alias and adapted interpreter let
    # that child execute correctly; missing them caused repeated forks.
    # Keep the SDK's QR/account binding flow enabled. --noqr switches this
    # build into an interactive account helper, which cannot use /dev/null.
    # Suppress console output through the redirection, not that SDK flag.
    "$APP/qdc-config-store" seed || { echo '花生壳配置无效，请在本地页面恢复绑定。' >&2; return 1; }
    (cd "$APP"; LD_LIBRARY_PATH="$APP/lib" exec nohup "$APP/phtunnel" \
        --rpc --rpcport 16063 --config "$RUN/module-oray.json" \
        --log /dev/null --pid "$RUN/module-oray.pid" --forward 127.0.0.1:8788 \
        </dev/null >/dev/null 2>&1) &
    remember "$!" "$RUN/standalone-tunnel.pid"
    sleep 1
    # A policy change may overlap process startup. The worker will reconcile
    # it, and the next supervisor pass must never restart a saved OFF policy.
    alive "$RUN/standalone-tunnel.pid"
}
events() {
    if alive "$RUN/events.pid"; then
        cmp -s /proc/"$task_pid"/exe "$ROOT/www/cgi-bin/qdc-comm" && return 0
        kill "$task_pid"
        count=0
        while alive "$RUN/events.pid"; do
            count=$((count+1)); [ "$count" -lt 60 ] || return 1
            sleep 0.1
        done
    fi
    nohup setsid "$ROOT/www/cgi-bin/qdc-comm" watch </dev/null >/dev/null 2>&1 &
    remember "$!" "$RUN/events.pid"
    sleep 1; alive "$RUN/events.pid"
}
network_watch_task() {
    executable=$(readlink /proc/"$1"/exe 2>/dev/null) || return 1
    case "$executable" in "$APP/qdc-network-watch"|"$APP/qdc-network-watch (deleted)") return 0;; *) return 1;; esac
}
network_watch() {
    [ -x "$APP/qdc-network-watch" ] || return 1
    if alive "$RUN/network-watch.pid"; then
        cmp -s /proc/"$task_pid"/exe "$APP/qdc-network-watch" && return 0
        network_watch_task "$task_pid" || return 1
        kill "$task_pid"
        count=0
        while alive "$RUN/network-watch.pid"; do
            count=$((count+1)); [ "$count" -lt 30 ] || return 1
            sleep 0.1
        done
    fi
    nohup setsid "$APP/qdc-network-watch" </dev/null > "$RUN/network-watch.log" 2>&1 &
    remember "$!" "$RUN/network-watch.pid"
    sleep 1; alive "$RUN/network-watch.pid"
}
network_reconnect() (
    # Network recovery must never override an OFF policy or race a settings
    # worker. FD 9 is closed for the newly launched client and its children.
    network_reason=${1:-}
    case "$network_reason" in network-restored|address-changed) ;; *) return 2;; esac
    [ ! -f "$RUN/standalone-maintenance" ] || return 75
    exec 9>"$RUN/data.lock"
    flock -n 9 || return 75
    [ ! -f "$RUN/standalone-maintenance" ] || return 75
    [ ! -e "$RUN/data-job" ] && ! tunnel_pending || return 75
    tunnel_policy || return 1
    [ "$tunnel_enabled" = 1 ] && [ ! -f "$RUN/standalone-tunnel-blocked" ] || return 3
    if [ -e "$ROOT/private/data-policy" ]; then
        data_enabled=$(cat "$ROOT/private/data-policy") || return 1
        case "$data_enabled" in 0) return 3;; 1) ;; *) return 1;; esac
    fi
    [ -f "$ROOT/private/standalone-enabled" ] || return 3
    # Leave an active call intact. Keep the event pending and retry locally
    # after it ends; this check itself does not use the cellular Internet.
    calls=$("$ROOT/www/cgi-bin/qdc-comm" call-list) || return 75
    case "$calls" in *'"ok":true'*'"calls":[]'*) ;; *) return 75;; esac
    "$APP/qdc-network-watch" --snapshot | grep -Fq '"online":true' || return 75
    verify
    stop_tunnel
    tunnel 1 9>&-
)
tunnel_supervised() (
    # Serialize periodic maintenance with network recovery and settings jobs.
    # A busy lock means another owner is already reconciling the client.
    exec 9>"$RUN/data.lock"
    flock -n 9 || return 0
    [ ! -f "$RUN/standalone-maintenance" ] && [ ! -e "$RUN/data-job" ] || return 0
    if [ -n "$(tunnel_tasks)" ]; then "$APP/qdc-config-store" checkpoint || true; fi
    tunnel 9>&-
)
once() {
    verify
    address
    data_connection || echo '数据连接尚未就绪，稍后重试。' >&2
    sh "$ROOT/control.sh" start
    if voice; then
        if ! alive "$RUN/audio.pid"; then
            origin; sh "$ROOT/control.sh" audio-start "$public_origin"
        fi
    else echo '语音尚未就绪，网页和穿透仍保持启动。' >&2; fi
    events || echo '事件监听尚未就绪，稍后重试。' >&2
    entry
    "$ROOT/www/cgi-bin/api" --usb-compat-reconcile || echo 'USB 漫游兼容尚未恢复，请在本地网页核对。' >&2
    network_watch || echo '本地网络监听尚未启动，稍后重试。' >&2
    tunnel_supervised
}
supervisor_cleanup() {
    # A previous supervisor's delayed TERM trap must not stop a replacement
    # supervisor's iteration after a service restart.
    if alive "$RUN/standalone-once.pid" && [ "$task_pid" = "${iteration_pid:-}" ]; then
        kill "$task_pid" 2>/dev/null || true
        rm -f "$RUN/standalone-once.pid"
    fi
    exit 0
}
supervise() {
    # The boot hook returns immediately; all slow work happens here.
    trap supervisor_cleanup TERM INT
    while [ -f "$ROOT/private/standalone-enabled" ]; do
        # Stop/start/update can establish this marker before replacing WWW.
        if [ ! -f "$RUN/standalone-maintenance" ]; then
            sh "$APP/service.sh" once > "$RUN/standalone-current.log" 2>&1 &
            iteration_pid=$!
            remember "$iteration_pid" "$RUN/standalone-once.pid"
            wait "$iteration_pid" || true
            rm -f "$RUN/standalone-once.pid"
        fi
        sleep 20
    done
}
start() {
    verify
    [ -f "$ROOT/private/standalone-enabled" ] || { echo '独立启动尚未启用。' >&2; return 1; }
    alive "$RUN/standalone-supervisor.pid" && return 0
    # Detach the session as well as ignoring HUP: the maintenance ADB shell
    # must not own the persistent supervisor's session/process group.
    nohup setsid sh "$APP/service.sh" supervise </dev/null > "$RUN/standalone-supervisor.log" 2>&1 &
    remember "$!" "$RUN/standalone-supervisor.pid"
    sleep 1
    alive "$RUN/standalone-supervisor.pid"
}
stop_owned() {
    # stop supervisor first, then stop its owned tunnel family.
    if alive "$RUN/standalone-supervisor.pid"; then kill "$task_pid"; fi
    rm -f "$RUN/standalone-supervisor.pid"
    if alive "$RUN/network-watch.pid"; then
        if network_watch_task "$task_pid"; then kill "$task_pid"; fi
    fi
    rm -f "$RUN/network-watch.pid"
    if alive "$RUN/standalone-once.pid"; then kill "$task_pid" 2>/dev/null || true; fi
    rm -f "$RUN/standalone-once.pid"
    stop_tunnel
    if alive "$RUN/standalone-entry.pid"; then kill "$task_pid"; fi
    rm -f "$RUN/standalone-entry.pid"
    if alive "$RUN/events.pid"; then kill "$task_pid"; fi
    rm -f "$RUN/events.pid"
    # Keep the existing web/audio service, active data link and drivers intact.
}
case "${1:-}" in
    check) verify; echo 'standalone prerequisites verified' ;;
    once) once ;;
    entry-start) verify; entry ;;
    events-start) verify; events ;;
    tunnel-start) verify; tunnel ;;
    tunnel-stop) stop_tunnel ;;
    tunnel-status) tunnel_status ;;
    tunnel-apply) verify; tunnel 1 ;;
    network-watch-start) verify; network_watch ;;
    tunnel-network-reconnect) network_reconnect "${2:-}" ;;
    start) start ;;
    stop) stop_owned ;;
    supervise) supervise ;;
    *) echo 'usage: service.sh {check|once|entry-start|events-start|tunnel-start|start|stop|supervise}' >&2; exit 2 ;;
esac

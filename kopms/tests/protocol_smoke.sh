#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: protocol_smoke.sh compositor wayland-client kopms-client" >&2
    exit 2
fi

if [ -z "${DISPLAY:-}" ]; then
    echo "SKIP: nested KOPMS smoke test requires DISPLAY" >&2
    exit 77
fi

runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/kopms-runtime.XXXXXX")
server_log=$(mktemp "${TMPDIR:-/tmp}/kopms-server.XXXXXX")
client_log=$(mktemp "${TMPDIR:-/tmp}/kopms-client.XXXXXX")
server_pid=""

cleanup() {
    if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$runtime_dir" "$server_log" "$client_log"
}
trap cleanup EXIT HUP INT TERM

env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$runtime_dir" KOP_LOG=debug \
    "$1" kopms-ci 3 >"$server_log" 2>&1 &
server_pid=$!

ready=0
i=0
while [ "$i" -lt 100 ]; do
    if [ -S "$runtime_dir/kopms-ci" ] && [ -S "$runtime_dir/kopms-ci.bus" ]; then
        ready=1
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.05
done

if [ "$ready" -ne 1 ]; then
    echo "KOPMS compositor did not create its Wayland and BUS sockets" >&2
    sed -n '1,160p' "$server_log" >&2 || true
    exit 1
fi

env XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY=kopms-ci \
    "$2" 1 >"$client_log" 2>&1
grep -q '握手完成' "$client_log"
grep -Eq '提交 [1-9][0-9]* 帧' "$client_log"

env XDG_RUNTIME_DIR="$runtime_dir" \
    "$3" "$runtime_dir/kopms-ci.bus" >>"$client_log" 2>&1
grep -q 'KOPMS-C handshake complete' "$client_log"

wait "$server_pid"
server_pid=""
grep -q 'xdg_toplevel 创建，已发送 configure' "$server_log"
grep -q 'ack_configure serial=' "$server_log"
grep -q 'surface commit ' "$server_log"
grep -q '客户端正常断开' "$server_log"
grep -q 'KOPMS-C session=.*handshake complete' "$server_log"

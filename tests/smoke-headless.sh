#!/usr/bin/env bash
#
# tests/smoke-headless.sh
#
# Env knobs:
#   VERBOSE=1   dump the daemon log on PASS too (default: only on FAIL)
#   KEEP_TMP=1  leave the tempdir behind for inspection (default: rm)
# ──────────────────────────────────────────────────────────────────────────

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# DAEMON_BIN / REMOTE_BIN env vars override the binary paths (used by
# tests/sanitize-smoke.sh to point at build/asan/ or build/tsan/).  When set,
# the caller owns the build state — we skip auto-build to avoid clobbering
# the sanitized binaries with a normal-flavor rebuild.
DAEMON="${DAEMON_BIN:-$TREE_ROOT/build/visualizer-daemon}"
REMOTE="${REMOTE_BIN:-$TREE_ROOT/build/visualizer-remote}"
EXTERNAL_BIN=0
[[ -n "${DAEMON_BIN:-}" || -n "${REMOTE_BIN:-}" ]] && EXTERNAL_BIN=1

# Derive APP_ID from the daemon's binary name.
APP_ID="$(basename "$DAEMON")"

log()  { printf '[smoke] %s\n' "$*"; }
fail() { printf '[smoke] FAIL: %s\n' "$*" >&2; }

# Only auto-build when paths weren't externally specified.  If a caller set
# DAEMON_BIN/REMOTE_BIN, they're driving the build (e.g. sanitize-smoke.sh
# already ran `make SAN=asan`)
if (( EXTERNAL_BIN == 0 )); then
    if [[ ! -x "$DAEMON" ]] || [[ ! -x "$REMOTE" ]]; then
        log "building (make)"
        if ! (cd "$TREE_ROOT" && make); then
            fail "build error"; exit 1
        fi
    fi
else
    if [[ ! -x "$DAEMON" ]]; then
        fail "DAEMON_BIN=$DAEMON does not exist or is not executable"; exit 1
    fi
    if [[ ! -x "$REMOTE" ]]; then
        fail "REMOTE_BIN=$REMOTE does not exist or is not executable"; exit 1
    fi
fi

# ─── Isolated runtime ─────────────────────────────────────────────────────
TMP="$(mktemp -d -t visualizer-smoke.XXXXXX)"
DAEMON_PID=""
DAEMON_LOG="$TMP/daemon.log"

dump_log() {
    if [[ -f "$DAEMON_LOG" ]]; then
        echo "[smoke] --- daemon log ---"
        sed 's/^/  | /' "$DAEMON_LOG"
        echo "[smoke] --- end log ---"
    fi
}

cleanup() {
    local rc=$?
    if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -KILL "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    if (( rc != 0 )) || [[ -n "${VERBOSE:-}" ]]; then
        dump_log
    fi
    if [[ -z "${KEEP_TMP:-}" ]]; then
        rm -rf "$TMP"
    else
        echo "[smoke] tempdir kept: $TMP"
    fi
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$TMP/run"
export XDG_CONFIG_HOME="$TMP/config"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/$APP_ID"
chmod 0700 "$XDG_RUNTIME_DIR"

CONFIG="$TMP/smoke.conf"
mkdir -p "$TMP/empty-presets"
cat > "$CONFIG" <<EOF
# Smoke-harness config — minimal, isolation-safe.
# audio.port: the daemon validates 1..65535, so we pick a high
# fixed port — collision-risk is vanishingly small and the log says clearly
# if it ever happens. 127.0.0.1 binds local-only (no firewall friction).
audio.port=49100
audio.addr=127.0.0.1
# Empty preset dir — the daemon tolerates zero presets. Decouples the smoke check
# from the host's presets install state.
presets.dir=$TMP/empty-presets
EOF

# ─── Launch ───────────────────────────────────────────────────────────────
log "launching: $DAEMON --display headless -c <config> -d"
"$DAEMON" --display headless -c "$CONFIG" -d > "$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!

# Poll for the IPC socket (ipc.c binds, then listens).
SOCK="$XDG_RUNTIME_DIR/${APP_ID}.sock"
SOCK_DEADLINE=$((SECONDS + 5))
while [[ ! -S "$SOCK" ]]; do
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        wait "$DAEMON_PID"; RC=$?
        fail "daemon exited before opening socket (status $RC)"
        exit 1
    fi
    if (( SECONDS >= SOCK_DEADLINE )); then
        fail "socket did not appear within 5s ($SOCK)"
        exit 1
    fi
    sleep 0.1
done
log "socket up at $SOCK"

# ─── IPC: a query and a state-changer ─────────────────────────────────────
log "remote: info"
if ! "$REMOTE" info >/dev/null; then
    fail "'info' returned non-zero"; exit 1
fi

log "remote: next"
if ! "$REMOTE" next >/dev/null; then
    fail "'next' returned non-zero"; exit 1
fi

# Let frames render. The render thread emits "[render] frame N presented"
# for N=1..5 when -d is set. Under a sanitizer on a software rasterizer the
# first frame can take several seconds to warm up (EGL and GL init), so poll
# with a deadline instead of a fixed sleep. This exits the instant a frame
# lands on a fast build, tolerates the slow sanitizer path, and only fails if
# nothing renders within the window.
FRAME_DEADLINE=$((SECONDS + 30))
until grep -q '\[render\] frame' "$DAEMON_LOG"; do
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        wait "$DAEMON_PID"; RC=$?
        fail "daemon exited before rendering a frame (status $RC)"
        exit 1
    fi
    if (( SECONDS >= FRAME_DEADLINE )); then
        fail "no '[render] frame' lines within 30s - render loop never ran"
        exit 1
    fi
    sleep 0.2
done
log "frames rendered (saw [render] frame in log)"

# ─── Shutdown via IPC ─────────────────────────────────────────────────────
log "remote: quit"
"$REMOTE" quit >/dev/null 2>&1 || true   # connection drop is normal here

# Watchdog: bound the wait so a hang reports clearly.
( sleep 5; kill -KILL "$DAEMON_PID" 2>/dev/null ) &
WATCHDOG=$!

wait "$DAEMON_PID"; RC=$?
# Tear down the watchdog, idempotently.
kill "$WATCHDOG" 2>/dev/null || true
wait "$WATCHDOG" 2>/dev/null || true
DAEMON_PID=""   # disarm cleanup's kill

if (( RC == 137 )); then
    fail "daemon hung; watchdog SIGKILLed it after 5s"
    exit 1
fi
if (( RC != 0 )); then
    fail "daemon exited with status $RC"
    exit 1
fi

log "PASS"
exit 0

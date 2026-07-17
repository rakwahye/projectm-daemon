#!/usr/bin/env bash
#
# tests/sanitize-smoke.sh — P0.2 sanitizer + teardown check.
#
# Builds the daemon under ASan or TSan (Makefile: SAN=asan|tsan, builds land
# in build/asan/ or build/tsan/), smoke harness against the sanitized binary.
#   - ASan: no use-after-free, no buffer overruns, no leaks at exit.
#   - TSan: no data races, no thread-leak warnings.
#
# Pass: exit 0.  Fail: exit 1, with the daemon log (containing the sanitizer
# report) printed by the underlying smoke harness via VERBOSE=1.
# 
# Usage:
#   tests/sanitize-smoke.sh asan
#   tests/sanitize-smoke.sh tsan

# ─── Sanitizer env ───────────────────────────────────────────────────────
# The daemon inherits these through the smoke harness's foreground launch.
#
# halt_on_error=1   — exit on first detected issue (clear failure point)
# abort_on_error=0  — do not dump core (CI / scratch dir hygiene)
# exitcode=66       — distinctive non-zero so a real crash vs a sanitizer
#                     fire can be told apart at a glance from exit status
# print_stacktrace  — include stacks in ASan reports (always useful here)
# detect_leaks=1    — turn on LSan at process exit (catches teardown leaks
#                     in worker threads / GBM / EGL / DRM resources, which
#                     is what the packet asks for)
# suppressions=...  — point each sanitizer at its baseline file
# ─────────────────────────────────────────────────────────────────────────

set -u
set -o pipefail

usage() {
    cat >&2 <<EOF
usage: $(basename "$0") <asan|tsan>

Exit:
  0  clean — no leaks/races/UAFs detected, clean teardown
  1  sanitizer fired OR smoke scenario failed (see daemon log above)
  2  argument / usage error
EOF
}

if [[ $# -ne 1 ]]; then
    usage; exit 2
fi
SAN_FLAVOR="$1"
case "$SAN_FLAVOR" in
    asan|tsan) ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown sanitizer '$SAN_FLAVOR' (want asan or tsan)" >&2
       usage; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[$SAN_FLAVOR] building: make SAN=$SAN_FLAVOR"
if ! (cd "$TREE_ROOT" && make SAN="$SAN_FLAVOR"); then
    echo "[$SAN_FLAVOR] FAIL: build error" >&2
    exit 1
fi

BUILD_DIR="$TREE_ROOT/build/$SAN_FLAVOR"
DAEMON="$BUILD_DIR/${APP_ID:-visualizer-daemon}"
REMOTE="$BUILD_DIR/${REMOTE_ID:-visualizer-remote}"

if [[ ! -x "$DAEMON" ]] || [[ ! -x "$REMOTE" ]]; then
    echo "[$SAN_FLAVOR] FAIL: expected binaries missing in $BUILD_DIR" >&2
    exit 1
fi

case "$SAN_FLAVOR" in
    asan)
        export ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_leaks=1:print_stacktrace=1:exitcode=66:symbolize=1"
        export LSAN_OPTIONS="exitcode=66:print_suppressions=1:suppressions=$SCRIPT_DIR/lsan.supp"
        ;;
    tsan)
        export TSAN_OPTIONS="halt_on_error=1:abort_on_error=0:exitcode=66:second_deadlock_stack=1:print_suppressions=1:suppressions=$SCRIPT_DIR/tsan.supp"
        ;;
esac

# VERBOSE=1 makes smoke-headless.sh always dump the daemon log; the sanitizer
# report lands in that log on failure, so this is how we see it.
export DAEMON_BIN="$DAEMON"
export REMOTE_BIN="$REMOTE"
export VERBOSE=1

echo "[$SAN_FLAVOR] running smoke against $DAEMON"
echo "[$SAN_FLAVOR] ─────────────────────────────────────────────────────────"
if "$SCRIPT_DIR/smoke-headless.sh"; then
    echo "[$SAN_FLAVOR] ─────────────────────────────────────────────────────────"
    echo "[$SAN_FLAVOR] PASS — no sanitizer reports, clean teardown"
    exit 0
fi
echo "[$SAN_FLAVOR] ─────────────────────────────────────────────────────────"
echo "[$SAN_FLAVOR] FAIL — see daemon log above for the sanitizer report." >&2
exit 1

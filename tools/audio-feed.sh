#!/bin/sh
# audio-feed.sh - feed system audio to the daemon (low-latency)
#
# Usage: audio-feed.sh <monitor-source-index>
#   Run 'pactl list short sources' to find your monitor index.
#   Override: VISUALIZER_AUDIO_PORT=9100 VISUALIZER_AUDIO_HOST=127.0.0.1 LATENCY_MS=10
#
# Requires: parec (pipewire-pulse), nc (gnu/openbsd netcat)

if [ -z "$1" ]; then
    printf 'Usage: %s <monitor-source-index>\n' "$0" >&2
    printf "  Run 'pactl list short sources' to find your monitor index.\n" >&2
    exit 1
fi

PORT=${VISUALIZER_AUDIO_PORT:-9100}
HOST=${VISUALIZER_AUDIO_HOST:-127.0.0.1}
MONITOR="$1"
RATE=48000
CHANNELS=2
LATENCY_MS=${LATENCY_MS:-10}

FIFO=$(mktemp -u /tmp/audio-feed.XXXXXX)
mkfifo "$FIFO"

PAREC_PID=""
cleanup() {
    trap - INT TERM EXIT
    [ -n "$PAREC_PID" ] && kill "$PAREC_PID" 2>/dev/null
    wait "$PAREC_PID" 2>/dev/null
    rm -f "$FIFO"
    exit 0
}
trap cleanup INT TERM EXIT

while true; do
    parec --format=float32le \
          --rate="$RATE" \
          --channels="$CHANNELS" \
          --latency-msec="$LATENCY_MS" \
          --monitor-stream="$MONITOR" \
          --raw 2>/dev/null > "$FIFO" &
    PAREC_PID=$!

    nc -N "$HOST" "$PORT" 2>/dev/null < "$FIFO"

    kill "$PAREC_PID" 2>/dev/null
    wait "$PAREC_PID" 2>/dev/null
    PAREC_PID=""
    sleep 1
done

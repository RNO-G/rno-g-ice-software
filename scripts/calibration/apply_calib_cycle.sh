#!/bin/sh
# Alternates calib.channel between fiber0 and fiber1 on each invocation.
# State is persisted in /rno-g/var/calib_channel.state

STATE_FILE=/rno-g/var/calib_channel.state
SCRIPT=$(dirname "$0")/apply_acq_overrides.py

if [ -f "$STATE_FILE" ] && [ "$(cat "$STATE_FILE")" = "fiber0" ]; then
    CHANNEL=fiber1
else
    CHANNEL=fiber0
fi

# extract the -O output path from the arguments
OUTPUT=""
prev=""
for arg in "$@"; do
    if [ "$prev" = "-O" ]; then
        OUTPUT="$arg"
    fi
    prev="$arg"
done

python3 "$SCRIPT" "$@" --set calib.channel="$CHANNEL" || exit 1

# run config checker on calib cfg files
if [ -n "$OUTPUT" ] && ! /rno-g/bin/check-rno-g-config acq "$OUTPUT" > /dev/null 2>&1; then
    echo "check-rno-g-config failed, removing $OUTPUT" >&2
    rm -f "$OUTPUT"
    exit 1
fi

echo "$CHANNEL" > "$STATE_FILE"

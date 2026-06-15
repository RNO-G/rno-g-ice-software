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

python3 "$SCRIPT" "$@" --set calib.channel="$CHANNEL"

echo "$CHANNEL" > "$STATE_FILE"

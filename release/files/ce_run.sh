#!/bin/sh
# Runs ce_cores and the webserver in the foreground, stops both when one of them exits.
# Started by the ce.service systemd unit, but can be also run by hand (as root) for debugging:
#   cd /opt/ce/bin && sudo ./ce_run.sh
# Output of both apps goes to stdout (journal when running as service), each line prefixed by the app name.

set -eu

cd "$(dirname "$0")"        # both apps load ./.env, so they need to run from BIN_DIR

. ./.env

mkdir -p "$DATA_DIR" "$LOG_DIR" "$PID_DIR" "$SETTINGS_DIR" "$IKBD_VIRTUAL_DEVICES_PATH"
chmod 0700 "$DATA_DIR" "$IKBD_VIRTUAL_DEVICES_PATH"    # owner only - command socket and virtual ikbd devices

# Nothing else is running (systemd makes sure of that), so any PID files and
# sockets are leftovers of a crash and would make the apps think another
# instance is running.
rm -f "$PID_DIR"/*.pid "$DATA_DIR"/*sock*

# runtime copy of the config drive
if [ -d ./configdrive ] && [ -n "$CONFIG_DRIVE_PATH" ] && [ "$CONFIG_DRIVE_PATH" != "/" ]; then
    rm -rf "$CONFIG_DRIVE_PATH"
    cp -a ./configdrive "$CONFIG_DRIVE_PATH"
fi

CORE_PID=""
WEB_PID=""
FIFO_DIR=""
stopping=0

cleanup() {
    if [ "$stopping" -eq 1 ]; then
        return
    fi
    stopping=1
    if [ -n "$CORE_PID" ]; then kill -INT "$CORE_PID" 2>/dev/null || true; fi
    if [ -n "$WEB_PID" ];  then kill -INT "$WEB_PID"  2>/dev/null || true; fi
    wait || true
    if [ -n "$FIFO_DIR" ]; then rm -rf "$FIFO_DIR"; fi
}

trap 'cleanup; exit 0' INT TERM
trap cleanup EXIT

# The output goes through a FIFO, so $! is the app's PID (not the PID of the prefixing sed).
# stdbuf -oL: C stdio is line buffered only on a terminal, so without it the cores output would come late.
# sed -u: don't buffer the prefixed lines; also drop the cores' clear-screen escape codes and empty lines.
FIFO_DIR=$(mktemp -d)
mkfifo "$FIFO_DIR/cores" "$FIFO_DIR/web"
sed -u -e 's/\x1b\[[0-9;]*[HJ]//g' -e '/^\s*$/d' -e 's/^/[cores] /' <"$FIFO_DIR/cores" &
sed -u -e 's/^/[web]   /' <"$FIFO_DIR/web" &

stdbuf -oL -eL ./ce_cores.elf >"$FIFO_DIR/cores" 2>&1 &
CORE_PID=$!

./webserver >"$FIFO_DIR/web" 2>&1 &
WEB_PID=$!

# Stay up while both processes are running; stop the other if one exits.
# Exiting with an error lets systemd restart the whole thing.
while kill -0 "$CORE_PID" 2>/dev/null && kill -0 "$WEB_PID" 2>/dev/null; do
    sleep 1
done

echo "ce_cores or webserver has exited, stopping"
exit 1

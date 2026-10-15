#!/bin/sh
# Installs or upgrades CosmosEx from a release tarball, and sets it up as a systemd service.
#
# usage: sudo ./install.sh [ce-<version>-<arch>.tar.gz]
#            without tarball: uses the newest tarball for this machine found next to this script
#        sudo ./install.sh --uninstall [--purge]
#            --purge also removes settings, data and logs
#
# Upgrades keep the settings (SETTINGS_DIR). If you edited bin/.env, your version is kept,
# and the .env from the new release is saved next to it as .env.new.

set -eu

SERVICE=ce
UNIT_FILE=/etc/systemd/system/$SERVICE.service
CE_DIR=/opt/ce
BIN_DIR=$CE_DIR/bin

# check if running as root, and if not, execute this script with sudo
if [ "$(id -u)" != 0 ]; then
    exec sudo "$0" "$@"
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
HAS_SYSTEMD=0
if command -v systemctl >/dev/null && [ -d /run/systemd/system ]; then
    HAS_SYSTEMD=1
fi

#-------------
# stop CosmosEx if it's running - as service, or started by the old ce_start.sh
stop_ce() {
    if [ "$HAS_SYSTEMD" -eq 1 ] && systemctl is-active --quiet $SERVICE; then
        echo "Stopping $SERVICE service"
        systemctl stop $SERVICE
    fi

    if [ -x "$BIN_DIR/ce_stop.sh" ]; then
        (cd "$BIN_DIR" && ./ce_stop.sh) || true
    fi
}

# remove a dir taken from .env, but never an empty path or /
safe_rm() {
    if [ -n "$1" ] && [ "$1" != "/" ]; then
        rm -rf "$1"
    fi
}

# print value of a variable from a .env file
env_value() {
    (cd "$(dirname "$1")" && . "./$(basename "$1")" && eval "echo \"\${$2}\"")
}

#-------------
uninstall() {
    purge="$1"

    stop_ce

    # read the dirs before the .env is gone
    data_dir="" log_dir="" settings_dir="" pid_dir="" config_drive=""
    if [ -f "$BIN_DIR/.env" ]; then
        data_dir=$(env_value "$BIN_DIR/.env" DATA_DIR)
        log_dir=$(env_value "$BIN_DIR/.env" LOG_DIR)
        settings_dir=$(env_value "$BIN_DIR/.env" SETTINGS_DIR)
        pid_dir=$(env_value "$BIN_DIR/.env" PID_DIR)
        config_drive=$(env_value "$BIN_DIR/.env" CONFIG_DRIVE_PATH)
    fi

    if [ -f "$UNIT_FILE" ]; then
        echo "Removing $SERVICE service"
        if [ "$HAS_SYSTEMD" -eq 1 ]; then
            systemctl disable $SERVICE 2>/dev/null || true
        fi
        rm -f "$UNIT_FILE"
        if [ "$HAS_SYSTEMD" -eq 1 ]; then
            systemctl daemon-reload
        fi
    fi

    echo "Removing $BIN_DIR"
    rm -rf "$BIN_DIR"
    safe_rm "$config_drive"
    safe_rm "$pid_dir"

    if [ "$purge" -eq 1 ]; then
        for d in "$settings_dir" "$data_dir" "$log_dir"; do
            if [ -n "$d" ]; then
                echo "Removing $d"
                safe_rm "$d"
            fi
        done
    else
        echo "Kept settings, data and logs (use --uninstall --purge to remove them too)"
    fi

    rmdir "$CE_DIR" 2>/dev/null || true
    echo "CosmosEx uninstalled."
}

#-------------
# architecture of this machine, named the same way as the tarballs
machine_arch() {
    case "$(uname -m)" in
        x86_64)         if [ "$(getconf LONG_BIT)" = 32 ]; then echo i386; else echo amd64; fi ;;    # 64-bit kernel can run 32-bit userland
        i?86)           echo i386 ;;
        aarch64|arm64)  if [ "$(getconf LONG_BIT)" = 32 ]; then echo armhf; else echo arm64; fi ;;   # 64-bit kernel can run 32-bit userland
        armv6l|armv7l|armv8l)  echo armhf ;;   # armhf is built for ARMv6, so it covers Pi 1 / Zero too
        *)              uname -m ;;
    esac
}

install_ce() {
    tarball="$1"
    arch=$(machine_arch)

    # no tarball specified? find the newest one for this architecture next to this script
    if [ -z "$tarball" ]; then
        tarball=$(ls "$SCRIPT_DIR"/ce-*-"$arch".tar.gz 2>/dev/null | sort -V | tail -n 1)
        if [ -z "$tarball" ]; then
            echo "No ce-*-$arch.tar.gz found in $SCRIPT_DIR, specify the tarball to install." >&2
            exit 1
        fi
    fi

    if [ ! -f "$tarball" ]; then
        echo "File not found: $tarball" >&2
        exit 1
    fi

    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT

    tar -C "$work" -xzf "$tarball"
    pkg=$(find "$work" -mindepth 1 -maxdepth 1 -type d | head -n 1)

    if [ -z "$pkg" ] || [ ! -f "$pkg/ARCH" ] || [ ! -d "$pkg/bin" ]; then
        echo "$tarball doesn't look like a CosmosEx release tarball." >&2
        exit 1
    fi

    pkg_arch=$(cat "$pkg/ARCH")
    version=$(cat "$pkg/VERSION")

    if [ "$pkg_arch" != "$arch" ]; then
        echo "$tarball is for $pkg_arch, but this machine is $arch." >&2
        exit 1
    fi

    echo "Installing CosmosEx $version ($arch) to $BIN_DIR"

    stop_ce

    #-------------
    # .env: keep it if the user changed it, otherwise take the new one.
    # .env.dist is the untouched .env of the installed release, used to detect the changes.
    new_bin="$pkg/bin"
    cp "$new_bin/.env" "$new_bin/.env.dist"

    if [ -f "$BIN_DIR/.env" ]; then
        if [ -f "$BIN_DIR/.env.dist" ] && cmp -s "$BIN_DIR/.env" "$BIN_DIR/.env.dist"; then
            :   # not changed by user, use the new one
        elif cmp -s "$BIN_DIR/.env" "$new_bin/.env"; then
            :   # same as the new one anyway
        else
            echo "Keeping your modified $BIN_DIR/.env, the new one is saved as $BIN_DIR/.env.new"
            cp "$new_bin/.env" "$new_bin/.env.new"
            cp "$BIN_DIR/.env" "$new_bin/.env"
        fi
    fi

    #-------------
    # swap the new bin dir in place
    mkdir -p "$CE_DIR"
    rm -rf "$BIN_DIR.new" "$BIN_DIR.old"
    cp -a "$new_bin" "$BIN_DIR.new"
    chown -R root:root "$BIN_DIR.new"

    if [ -d "$BIN_DIR" ]; then
        mv "$BIN_DIR" "$BIN_DIR.old"
    fi
    mv "$BIN_DIR.new" "$BIN_DIR"
    rm -rf "$BIN_DIR.old"

    #-------------
    # service
    if [ "$HAS_SYSTEMD" -eq 1 ]; then
        sed "s|@BIN_DIR@|$BIN_DIR|g" "$pkg/ce.service" > "$UNIT_FILE"
        systemctl daemon-reload
        systemctl enable $SERVICE >/dev/null 2>&1
        systemctl restart $SERVICE
        echo "CosmosEx $version is installed and running as the '$SERVICE' service."
        echo "  status:  systemctl status $SERVICE"
        echo "  logs:    journalctl -u $SERVICE -f"
    else
        echo "CosmosEx $version is installed. No systemd found, so start it yourself:"
        echo "  cd $BIN_DIR && sudo ./ce_run.sh"
    fi

    port=$(env_value "$BIN_DIR/.env" WEBSERVER_PORT)
    echo "  web UI:  https://$(hostname):$port"
}

#-------------
case "${1:-}" in
    -h|--help)
        sed -n '2,10p' "$0"
        ;;
    --uninstall)
        purge=0
        if [ "${2:-}" = --purge ]; then purge=1; fi
        uninstall $purge
        ;;
    *)
        install_ce "${1:-}"
        ;;
esac

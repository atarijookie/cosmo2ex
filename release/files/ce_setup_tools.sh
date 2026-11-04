#!/bin/sh
# Sets up the programs CosmosEx's helpers need, by asking simple yes / no questions:
#   - ffmpeg      - for the video and audio players (CE_VIDEO.TTP, CE_AUDIO.TTP), used by ce_media
#   - Chrome      - headless, for the web browser (CE_BROWS.PRG), used by ce_browser
#   - Freedoom    - the game data for Doom (CE_DOOM.PRG), played by ce_doomgeneric (comes with CosmosEx)
# The config drive shows these Atari programs only when what they need is here.
#
# usage:  Linux:  sudo /opt/ce/bin/ce_setup_tools.sh
#         macOS:  ./ce_setup_tools.sh   (in the CosmosEx folder, not as root)
#
# What it can do:
#   ffmpeg:  from the system's packages (apt, dnf, pacman, zypper, apk / Homebrew on macOS), or on Linux in docker
#            instead (installs docker too, sets MEDIA_DOCKER_IMAGE in .env)
#   Chrome:  Chrome for Testing's chrome-headless-shell from Google (Linux x86-64, macOS), unpacked to the tools folder
#            next to the bin folder; on other Linux CPUs (Raspberry Pi, ...) Chromium from the system's packages.
#            Chrome doesn't need docker - ce_browser starts it as a normal program. On Linux it also adds a user
#            for it (Chrome must not run as root) and, on Ubuntu, the AppArmor rule Chrome's sandbox needs.
#   Freedoom: downloaded from its GitHub release (checked against its SHA-256), freedoom1.wad to the tools folder -
#            ce_media looks there (also for your own doom.wad / doom1.wad, if you put one there).
# The paths go to .env (MEDIA_FFMPEG, CHROME_PATH, ...), so it can be run again any time - it skips what's working.
# Enter takes the answer in capitals ([Y/n]: yes).

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ENV_FILE="$SCRIPT_DIR/.env"
OS=$(uname -s)
SERVICE=ce

FFMPEG_IMAGE=linuxserver/ffmpeg     # amd64 + arm64 only
BROWSER_USER_NEW=cebrowser
CFT_URL=https://googlechromelabs.github.io/chrome-for-testing/LATEST_RELEASE_STABLE
CFT_DL=https://storage.googleapis.com/chrome-for-testing-public
FREEDOOM_URL=https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
FREEDOOM_SHA256=3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59

#-------------
# helpers

say()  { printf '%s\n' "$*"; }
ok()   { printf '  OK: %s\n' "$*"; }
warn() { printf '  !! %s\n' "$*"; }

# ask "question" Y|N - returns 0 for yes. Reads the terminal, so it works also when piped.
ask() {
    if [ "$2" = Y ]; then hint="[Y/n]"; else hint="[y/N]"; fi
    while :; do
        printf '%s %s ' "$1" "$hint"
        if (exec </dev/tty) 2>/dev/null; then       # the terminal, also when this script is piped (subshell: a failed
                                                    # redirection would end the whole script in dash)
            read -r a </dev/tty || a=""
        else
            read -r a || a=""                       # no terminal: answers from stdin
        fi
        case "$a" in
            "")         [ "$2" = Y ]; return ;;
            y|Y|yes)    return 0 ;;
            n|N|no)     return 1 ;;
        esac
    done
}

# value of KEY in .env, as written there (only uncommented lines)
env_get() {
    sed -n "s/^$1=//p" "$ENV_FILE" | tail -n 1 | sed 's/^"\(.*\)"$/\1/'
}

# KEY=VALUE in .env: replaces the KEY= line, else the first #KEY= line, else adds it at the end
env_set() {
    tmp="$ENV_FILE.tmp.$$"
    v="$2"
    case "$v" in
        *" "*)  v="\"$v\"" ;;                          # e.g. a macOS folder with a space in its name
    esac
    awk -v k="$1" -v v="$v" '
        BEGIN { done = 0 }
        { lines[NR] = $0 }
        $0 ~ "^" k "=" && !set   { set = NR }
        $0 ~ "^#" k "=" && !cmt  { cmt = NR }
        END {
            at = set ? set : cmt
            for (i = 1; i <= NR; i++) print (i == at) ? k "=" v : lines[i]
            if (!at) print k "=" v
        }' "$ENV_FILE" > "$tmp" && cat "$tmp" > "$ENV_FILE"       # cat: keeps owner and permissions of .env
    rm -f "$tmp"
    ok "$1=$2 in .env"
}

# KEY in .env commented out again
env_unset() {
    if grep -q "^$1=" "$ENV_FILE"; then
        tmp="$ENV_FILE.tmp.$$"
        sed "s/^$1=/#$1=/" "$ENV_FILE" > "$tmp" && cat "$tmp" > "$ENV_FILE"
        rm -f "$tmp"
    fi
}

have() { command -v "$1" >/dev/null 2>&1; }

#-------------
# system packages (Linux)

PM=""
detect_pm() {
    for p in apt-get dnf pacman zypper apk; do
        if have $p; then PM=$p; return; fi
    done
}

PM_UPDATED=0
pkg_install() {      # pkg_install name... - 0 when installed; stdin closed, so they don't eat the answers
    pkg_install_ "$@" </dev/null
}
pkg_install_() {
    case "$PM" in
        apt-get)
            if [ "$PM_UPDATED" -eq 0 ]; then apt-get update; PM_UPDATED=1; fi
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@" ;;
        dnf)    dnf install -y "$@" ;;
        pacman) pacman -S --needed --noconfirm "$@" ;;
        zypper) zypper --non-interactive install "$@" ;;
        apk)    apk add "$@" ;;
        *)      return 1 ;;
    esac
}

machine_arch() {
    case "$(uname -m)" in
        x86_64|amd64)   if [ "$OS" = Linux ] && [ "$(getconf LONG_BIT)" = 32 ]; then echo i386; else echo amd64; fi ;;
        aarch64|arm64)  if [ "$OS" = Linux ] && [ "$(getconf LONG_BIT)" = 32 ]; then echo armhf; else echo arm64; fi ;;
        armv*)          echo armhf ;;
        i?86)           echo i386 ;;
        *)              uname -m ;;
    esac
}
ARCH=$(machine_arch)

download() {        # download url file
    say "  downloading $1"
    curl -fL -# --retry 3 --connect-timeout 20 -o "$2" "$1"
}

#-------------
# start

case "$OS" in
    Linux)
        if [ "$(id -u)" != 0 ]; then
            exec sudo "$0" "$@"
        fi
        TOOLS_DIR=$(dirname "$SCRIPT_DIR")/tools        # /opt/ce/tools - kept when CosmosEx is updated
        detect_pm
        ;;
    Darwin)
        if [ "$(id -u)" = 0 ]; then
            say "Run this without sudo on macOS (Homebrew doesn't run as root)."
            exit 1
        fi
        TOOLS_DIR="$SCRIPT_DIR/tools"
        ;;
    *)
        say "This script is for Linux and macOS. On Windows run ce_setup_tools.bat."
        exit 1
        ;;
esac

if [ ! -f "$ENV_FILE" ]; then
    say "No .env next to this script ($ENV_FILE) - run it from the CosmosEx bin folder."
    exit 1
fi

if ! have curl && [ "$OS" = Linux ]; then
    pkg_install curl ca-certificates >/dev/null 2>&1 || true
fi

say "CosmosEx tools setup"
say "===================="
say "The video / audio players and the web browser need programs which are not part of CosmosEx."
say "This asks before installing anything. Enter = the answer in capitals."
say ""

CHANGED=0

#-------------
# ffmpeg

ffmpeg_works() {    # in .env or in PATH, or the docker image
    img=$(env_get MEDIA_DOCKER_IMAGE)
    if [ -n "$img" ]; then
        if have docker && docker image inspect "$img" >/dev/null 2>&1; then
            ok "ffmpeg: docker image $img"
            return 0
        fi
        if have docker; then
            ok "ffmpeg: docker image $img (downloaded when a player starts the first time)"
            return 0
        fi
        warn "MEDIA_DOCKER_IMAGE=$img is set in .env, but docker is not installed"
        return 1
    fi
    f=$(env_get MEDIA_FFMPEG)
    if [ -n "$f" ]; then
        if [ -x "$f" ]; then ok "ffmpeg: $f"; return 0; fi
        warn "MEDIA_FFMPEG=$f is set in .env, but it's not there"
        return 1
    fi
    if have ffmpeg && have ffprobe; then
        ok "ffmpeg: $(command -v ffmpeg)"
        return 0
    fi
    return 1
}

ffmpeg_native() {
    if [ "$OS" = Darwin ]; then
        if ! have brew; then
            for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do
                if [ -x "$b" ]; then PATH="$(dirname "$b"):$PATH"; fi
            done
        fi
        if ! have brew; then
            say "  ffmpeg is installed with Homebrew (https://brew.sh), the usual package manager on macOS."
            ask "  Install Homebrew first? It asks for your password." Y || return 1
            /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" </dev/tty || return 1
            for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do
                if [ -x "$b" ]; then PATH="$(dirname "$b"):$PATH"; fi
            done
            have brew || return 1
        fi
        ask "  Install ffmpeg with Homebrew?" Y || return 1
        brew install ffmpeg || return 1
        # CosmosEx.command may not have Homebrew in its PATH - full paths in .env
        p=$(brew --prefix)/bin
        env_unset MEDIA_DOCKER_IMAGE
        env_set MEDIA_FFMPEG "$p/ffmpeg"
        env_set MEDIA_FFPROBE "$p/ffprobe"
        return 0
    fi

    if [ -z "$PM" ]; then
        warn "no known package manager (apt-get, dnf, pacman, zypper, apk) - install ffmpeg yourself"
        return 1
    fi
    ask "  Install ffmpeg from the system's packages ($PM)?" Y || return 1
    if ! pkg_install ffmpeg; then
        if [ "$PM" = dnf ]; then
            pkg_install ffmpeg-free || return 1         # Fedora without RPM Fusion
        else
            return 1
        fi
    fi
    have ffmpeg && have ffprobe || return 1
    env_unset MEDIA_DOCKER_IMAGE
    env_unset MEDIA_FFMPEG
    env_unset MEDIA_FFPROBE
    return 0
}

docker_ready() {
    if ! have docker; then
        ask "  Docker is not installed. Install it from the system's packages ($PM)?" Y || return 1
        case "$PM" in
            apt-get)    pkg_install docker.io ;;
            dnf)        pkg_install moby-engine || pkg_install docker ;;
            *)          pkg_install docker ;;
        esac || return 1
        have docker || return 1
    fi
    if ! docker info >/dev/null 2>&1; then
        say "  starting docker"
        if have systemctl; then
            systemctl enable --now docker >/dev/null 2>&1 || true
        elif have rc-service; then
            rc-update add docker default >/dev/null 2>&1 || true
            rc-service docker start || true
        fi
        sleep 2
        docker info >/dev/null 2>&1 || { warn "docker doesn't run (docker info failed)"; return 1; }
    fi
    return 0
}

ffmpeg_docker() {
    if [ "$OS" != Linux ]; then
        return 1
    fi
    if [ "$ARCH" != amd64 ] && [ "$ARCH" != arm64 ]; then
        return 1                                        # the image is only for these
    fi
    ask "  Use ffmpeg in docker instead (image $FFMPEG_IMAGE, about 1 GB)?" N || return 1
    docker_ready || return 1
    env_unset MEDIA_FFMPEG
    env_unset MEDIA_FFPROBE
    env_set MEDIA_DOCKER_IMAGE "$FFMPEG_IMAGE"
    if ask "  Download the image now? (else when a player starts the first time)" Y; then
        docker pull "$FFMPEG_IMAGE" || warn "download failed - it's tried again when a player starts"
    fi
    return 0
}

say "1) ffmpeg - for CE_VIDEO.TTP and CE_AUDIO.TTP"
if ! ffmpeg_works; then
    if ffmpeg_native || ffmpeg_docker; then
        CHANGED=1
        ffmpeg_works >/dev/null && ok "ffmpeg is ready"
    else
        warn "no ffmpeg - the video and audio players won't work (run this again to install it)"
    fi
fi
say ""

#-------------
# Chrome

chrome_works() {
    c=$(env_get CHROME_PATH)
    if [ -n "$c" ]; then
        if [ -x "$c" ]; then ok "Chrome: $c"; return 0; fi
        warn "CHROME_PATH=$c is set in .env, but it's not there"
    fi
    return 1
}

# Chrome for Testing's chrome-headless-shell for this platform, to TOOLS_DIR
chrome_cft() {
    case "$OS/$ARCH" in
        Linux/amd64)    plat=linux64 ;;
        Darwin/arm64)   plat=mac-arm64 ;;
        Darwin/amd64)   plat=mac-x64 ;;
        *)              return 1 ;;
    esac
    ask "  Download Chrome headless shell from Google (about 120 MB) to $TOOLS_DIR?" Y || return 1

    if [ "$OS" = Linux ] && ! have unzip; then
        pkg_install unzip || { warn "can't install unzip"; return 1; }
    fi

    ver=$(curl -fsSL "$CFT_URL") || { warn "can't get the version of Chrome for Testing"; return 1; }
    zip="$TOOLS_DIR/chrome-headless-shell.zip"
    mkdir -p "$TOOLS_DIR"
    download "$CFT_DL/$ver/$plat/chrome-headless-shell-$plat.zip" "$zip" || { rm -f "$zip"; return 1; }
    rm -rf "$TOOLS_DIR/chrome-headless-shell-$plat"
    unzip -q -o "$zip" -d "$TOOLS_DIR" || { rm -f "$zip"; return 1; }
    rm -f "$zip"
    CHROME="$TOOLS_DIR/chrome-headless-shell-$plat/chrome-headless-shell"
    [ -x "$CHROME" ] || return 1
    ok "Chrome $ver in $TOOLS_DIR"

    if [ "$OS" = Linux ]; then
        chmod -R a+rX "$TOOLS_DIR"                      # ce_browser runs as a normal user
        chrome_linux_libs
    fi
    return 0
}

# the libraries chrome-headless-shell needs, which a server / minimal install may not have
chrome_linux_libs() {
    missing=$(ldd "$CHROME" 2>/dev/null | awk '/not found/ { print $1 }')
    if [ -z "$missing" ]; then
        return 0
    fi
    say "  Chrome needs libraries which are not installed: $(echo $missing)"
    case "$PM" in
        apt-get)
            pkg_install libnss3 libnspr4 libatk1.0-0 libatk-bridge2.0-0 libcups2 libdrm2 libdbus-1-3 libexpat1 \
                libxkbcommon0 libxcomposite1 libxdamage1 libxfixes3 libxrandr2 libgbm1 libasound2 libpango-1.0-0 \
                libcairo2 fontconfig fonts-liberation ;;
        dnf)    pkg_install nss atk at-spi2-atk cups-libs libdrm libxkbcommon libXcomposite libXdamage libXfixes \
                    libXrandr mesa-libgbm alsa-lib pango cairo fontconfig liberation-sans-fonts ;;
        pacman) pkg_install nss atk at-spi2-core libcups libdrm libxkbcommon libxcomposite libxdamage libxfixes \
                    libxrandr mesa alsa-lib pango cairo fontconfig ttf-liberation ;;
        zypper) pkg_install mozilla-nss libatk-1_0-0 libatk-bridge-2_0-0 libcups2 libdrm2 libxkbcommon0 \
                    libXcomposite1 libXdamage1 libXfixes3 libXrandr2 libgbm1 libasound2 libpango-1_0-0 libcairo2 \
                    fontconfig liberation-fonts ;;
    esac
    missing=$(ldd "$CHROME" 2>/dev/null | awk '/not found/ { print $1 }')
    if [ -n "$missing" ]; then
        warn "still missing: $(echo $missing) - install the packages with these libraries yourself"
    fi
}

# Chromium from the system's packages - for Linux CPUs Chrome for Testing doesn't exist for (ARM, 32-bit)
chrome_distro() {
    if [ "$OS" != Linux ] || [ -z "$PM" ]; then
        return 1
    fi
    ask "  Install Chromium from the system's packages ($PM)?" Y || return 1
    case "$PM" in
        apt-get)    pkg_install chromium fonts-liberation || pkg_install chromium-browser fonts-liberation ;;
        *)          pkg_install chromium ;;
    esac
    for c in /usr/bin/chromium /usr/bin/chromium-browser /usr/lib/chromium/chromium; do
        if [ -x "$c" ]; then
            if [ "$(readlink -f "$c")" = /snap/bin/chromium ] || [ "$(readlink -f "$c")" = /usr/bin/snap ]; then
                warn "$c is a snap - it may not work for ce_browser"
            fi
            CHROME=$c
            return 0
        fi
    done
    return 1
}

# Chrome must not run as root - when CosmosEx runs as root (the service), ce_browser needs a user to run as
browser_user() {
    if [ "$OS" != Linux ]; then
        return 0
    fi
    u=$(env_get BROWSER_USER)
    if [ -n "$u" ] && id "$u" >/dev/null 2>&1; then
        ok "ce_browser runs as user $u"
        return 0
    fi
    say "  CosmosEx runs as root, but Chrome must run as a normal user."
    ask "  Add user '$BROWSER_USER_NEW' for it (no password, can't log in)?" Y || {
        warn "set BROWSER_USER in .env to a normal user yourself"
        return 1
    }
    if ! id "$BROWSER_USER_NEW" >/dev/null 2>&1; then
        nologin=/usr/sbin/nologin
        [ -x "$nologin" ] || nologin=/sbin/nologin
        [ -x "$nologin" ] || nologin=/bin/false
        if have useradd; then
            useradd --system --create-home --home-dir "/var/lib/$BROWSER_USER_NEW" --shell "$nologin" "$BROWSER_USER_NEW"
        else
            adduser -S -D -h "/var/lib/$BROWSER_USER_NEW" -s "$nologin" "$BROWSER_USER_NEW"        # busybox (Alpine)
        fi || { warn "can't add the user"; return 1; }
    fi
    env_set BROWSER_USER "$BROWSER_USER_NEW"
    CHANGED=1
}

# Ubuntu 23.10+ lets only programs with an AppArmor profile use user namespaces - Chrome's sandbox needs them
chrome_apparmor() {
    if [ "$OS" != Linux ] || [ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns 2>/dev/null)" != 1 ]; then
        return 0
    fi
    case "$CHROME" in
        /usr/*) return 0 ;;                             # the distro's chromium brings its own profile
    esac
    prof=/etc/apparmor.d/cosmosex-chrome
    say "  This system restricts the sandbox Chrome uses (AppArmor) - adding a rule for $CHROME"
    cat > "$prof" <<EOF
# added by CosmosEx ce_setup_tools.sh - lets Chrome for ce_browser use its sandbox
abi <abi/4.0>,
include <tunables/global>

profile cosmosex-chrome $CHROME flags=(unconfined) {
  userns,
  include if exists <local/cosmosex-chrome>
}
EOF
    apparmor_parser -r "$prof" || warn "apparmor_parser failed - Chrome may not start"
}

say "2) Chrome (headless) - for CE_BROWS.PRG"
CHROME=""
if chrome_works; then
    CHROME=$(env_get CHROME_PATH)
elif chrome_cft || chrome_distro; then
    env_set CHROME_PATH "$CHROME"
    CHANGED=1
else
    CHROME=""
    warn "no Chrome - the web browser won't work (run this again to install it)"
fi
if [ -n "$CHROME" ] && [ "$OS" = Linux ]; then
    browser_user
    chrome_apparmor
fi
say ""

#-------------
# Doom: the engine comes with CosmosEx, the game data (a WAD) is downloaded

doom_wad() {        # the WAD ce_media would use, "" when none
    w=$(env_get MEDIA_DOOM_WAD)
    if [ -n "$w" ]; then
        [ -f "$w" ] && echo "$w"
        return
    fi
    for d in "$TOOLS_DIR" /usr/share/games/doom /usr/local/share/games/doom; do
        for n in doom.wad doom1.wad freedoom1.wad; do
            if [ -f "$d/$n" ]; then
                echo "$d/$n"
                return
            fi
        done
    done
}

sha256_of() {
    if have sha256sum; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

freedoom_download() {
    ask "  Download Freedoom (a free Doom game, about 25 MB) to $TOOLS_DIR?" Y || return 1
    if [ "$OS" = Linux ] && ! have unzip; then
        pkg_install unzip || { warn "can't install unzip"; return 1; }
    fi
    mkdir -p "$TOOLS_DIR"
    zip="$TOOLS_DIR/freedoom.zip"
    download "$FREEDOOM_URL" "$zip" || { rm -f "$zip"; return 1; }
    if [ "$(sha256_of "$zip")" != "$FREEDOOM_SHA256" ]; then
        rm -f "$zip"
        warn "the download doesn't match its checksum - not used"
        return 1
    fi
    unzip -q -o -j "$zip" '*/freedoom1.wad' -d "$TOOLS_DIR" || { rm -f "$zip"; return 1; }
    rm -f "$zip"
    chmod a+r "$TOOLS_DIR/freedoom1.wad"
    ok "Freedoom in $TOOLS_DIR/freedoom1.wad"
}

say "3) Doom - for CE_DOOM.PRG"
if [ ! -x "$SCRIPT_DIR/ce_doomgeneric" ] && [ -z "$(env_get MEDIA_DOOM_BIN)" ]; then
    warn "the Doom engine (ce_doomgeneric) is not in this CosmosEx folder - update CosmosEx"
elif [ -n "$(doom_wad)" ]; then
    ok "Doom: $(doom_wad)"
elif freedoom_download; then
    say "  CE_DOOM.PRG shows up on the config drive within a minute."
else
    warn "no Doom WAD - CE_DOOM.PRG stays hidden (run this again to download Freedoom)"
fi
say ""

#-------------
# restart

if [ "$CHANGED" -eq 1 ]; then
    if [ "$OS" = Linux ] && have systemctl && systemctl is-active --quiet $SERVICE 2>/dev/null; then
        if ask "Restart CosmosEx now, so it uses them?" Y; then
            systemctl restart $SERVICE && ok "restarted"
        fi
    else
        say "Restart CosmosEx, so it uses them."
    fi
fi
say "Done."

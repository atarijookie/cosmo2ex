#!/bin/sh
# Builds the Android floppy app into webserver/static/app/ce_floppy.apk, where the webserver serves it from, using the
# ce-android-builder docker image (android/Dockerfile). The APK runs on all phones, so this runs once, not per platform.
#
# VERSION env var is the version shown in the phone's app info (default: date + git hash, like release/build.sh).
#
# The APK is signed with android/ce_app.keystore, kept in git on purpose: phones install an update only if it has the same
# key, this way any build can update any other. The worst case of losing or replacing it: reinstall + scan the QR code.

set -eu

ANDROID_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$ANDROID_DIR/.." && pwd)
OUT="$ROOT/webserver/static/app"
IMAGE=ce-android-builder
VERSION=${VERSION:-$(date +%Y%m%d)-$(git -C "$ROOT" rev-parse --short HEAD)}

echo "Building the $IMAGE image"
docker build -t "$IMAGE" "$ANDROID_DIR"

mkdir -p "$OUT" "$ANDROID_DIR/.gradle-cache"

# built from a copy, so the sources stay clean; downloaded Gradle plugins are kept between the builds
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e VERSION="$VERSION" \
    -e ANDROID_USER_HOME=/tmp/.android \
    -e GRADLE_OPTS=-Duser.home=/tmp \
    -e GRADLE_USER_HOME=/gradle-cache \
    -v "$ANDROID_DIR":/src:ro \
    -v "$ANDROID_DIR/.gradle-cache":/gradle-cache \
    -v "$OUT":/out \
    "$IMAGE" \
    sh -c 'set -e
        mkdir /tmp/build
        cd /src && tar cf - --exclude=.gradle-cache . | tar xf - -C /tmp/build
        cd /tmp/build
        gradle --no-daemon --console=plain assembleRelease
        cp app/build/outputs/apk/release/app-release.apk /out/ce_floppy.apk'

ls -l "$OUT/ce_floppy.apk"

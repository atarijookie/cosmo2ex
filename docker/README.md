Before the first build (and after changing the Atari drivers): the config drive
drivers are gitignored build outputs, but the image needs them in
ce_cores/configdrive/drivers/ (the build fails with "missing file from .env"
otherwise). Build them with the m68k-atari-mint-gcc cross compiler and copy
them there (from the repo root):
make -C libacsiscsi
make -C ce_dd_bootsectors all
mkdir -p ce_cores/configdrive/drivers
cp ce_dd_bootsectors/ce_dd.bs ce_dd_bootsectors/ce_dd_l2.bs ce_dd_prg/ce_dd.prg ce_cores/configdrive/drivers/

To build docker image (from the repo root):
docker build -f docker/Dockerfile -t cosmo2ex .

To build the image for all platforms on one machine (also builds the drivers
above when m68k-atari-mint-gcc is installed):
docker/build_all.sh                             # all platforms
docker/build_all.sh linux/arm/v6 linux/arm64    # just some of them
Platforms: linux/amd64, linux/386 (32-bit PC), linux/arm64 (Pi 3/4/5/Zero 2 W
with 64-bit OS), linux/arm/v7 (Pi 2+ with 32-bit OS), linux/arm/v6 (Pi 1 /
Zero / Zero W). All of them end up under the one cosmo2ex tag, and one image
tarball per platform is saved to docker/out/. Copy it to the target and load
it there:
docker load -i cosmo2ex-arm-v6.tar.gz
Nothing is emulated: ce_cores and the webserver are cross compiled on the build
machine, and the platform images only get files copied in. Debian has no
ARMv6 base image, so linux/arm/v6 is a minimal Raspbian userland (just what
entrypoint.sh needs: sh, coreutils, sed, tini).

Preferred run (persists settings, exposes the host filesystem, host network
for UDP discovery broadcasts):
docker compose -f docker/docker-compose.yml up --build

Interactive run - ce_cores and webserver output in the same console, each
line prefixed with [cores] or [web]:
docker compose -f docker/docker-compose.yml run --rm ce
The same output is in docker logs when running with up -d.

Equivalent docker run:
docker run --rm --network host \
  -v /opt/ce/settings:/opt/ce/settings \
  -v /opt/ce/floppyImages:/opt/ce/floppyImages \
  -v /opt/ce/log:/var/log/ce \
  -e CE_LOG_LEVEL=3 \
  -v /:/mnt/hostdrive:rslave \
  -v /dev:/dev \
  --device-cgroup-rule='c 13:* rwm' \
  --device-cgroup-rule='b 8:* rwm' \
  cosmo2ex

Settings live on the host at /opt/ce/settings (same as SETTINGS_DIR in .env).
Override the host path with CE_SETTINGS_HOST=/your/path.

Floppy images downloaded by the webserver (when allowed in its Settings) are on
the host at /opt/ce/floppyImages, next to the settings (FLOPPY_IMAGES_DIR in
.env). Override the host path with CE_FLOPPY_HOST=/your/path.

Logs of ce_cores and the webserver are on the host at /opt/ce/log, next to the
settings (inside the container it is LOG_DIR=/var/log/ce from .env).
Override the host path with CE_LOG_HOST=/your/path.

Log level: the default is 3 (info). Set CE_LOG_LEVEL to change it:
0 = off, 1 = errors, 2 = + warnings, 3 = + info, 4 = + debug (most verbose).
For debug logs:
CE_LOG_LEVEL=4 docker compose -f docker/docker-compose.yml up --build
or with docker run: -e CE_LOG_LEVEL=4

Web UI password: the default is 'ce'. After the first login you have to set a
new password; its bcrypt hash is stored in SETTINGS_DIR/web_password_hash.
Forgot the password? Delete that file on the host and log in with 'ce' again.

HTTPS: the web UI is served over HTTPS on port 8080; plain http:// on the same
port redirects to https://. On first start a local CA (valid 20 years) is
created in SETTINGS_DIR/tls, and the server certificate (825 days) is issued
from it for the hostname and current IPs. The webserver checks it hourly and
re-issues it before it expires or when the IPs change, always from the same CA.
To get rid of browser warnings, download the CA from https://<box>:8080/ca.crt
(link on the login page) and install it once as a trusted root on your PC.
The CA is limited to local names and private IP ranges.

The host filesystem is mounted read-write at /mnt/hostdrive. Point GEM /
translated drive paths at /mnt/hostdrive/... in the web UI (for example
/mnt/hostdrive/home/user).

Discovery uses host networking (Linux). Devices broadcast UDP to port 7200;
the server unicasts the reply to the device on port 7201. Bridge port mapping
(-p 7200:7200/udp) does not receive LAN broadcasts. 8080 and 7300-7302 are
also bound directly on the host.

USB mice, keyboards and gamepads: IKBD lists input devices with libudev and
opens /dev/input/event* / js*. Host /dev is bind-mounted at the same path.
Char major 13 is allowed so newly plugged devices can be opened. Hotplug comes
from kernel uevents (host networking); if the host's /run/udev is mounted
read-only, udev events are used instead.

USB raw disks for HDD emulation (/dev/sd*): block major 8 is allowed.
WARNING: this includes any SATA/USB system disk of the host.

License: open design, non-commercial license - see LICENSE / the License section of the main README.md.

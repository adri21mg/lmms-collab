#!/usr/bin/env bash
# Installs the LMMS collaboration server on Ubuntu (24.04 or newer) / Debian (12 or newer) as a service that starts with
# the computer. Run it from the LMMS source folder:
#
#   sudo bash collab/deploy/install-ubuntu.sh                      # listens on every address
#   sudo bash collab/deploy/install-ubuntu.sh --listen 100.x.y.z   # only on one address (e.g. your Tailscale IP)
#
# Versions are always kept by the server. To also submit them to a Perforce (Helix Core) server, add:
#   --p4port 127.0.0.1:1666 --p4user lmms-collab --p4depot //depot/music
# (that user logs in once on this computer, see collab/deploy/README.md)
#
# Running it again updates the server; the settings of the last install are kept unless given again,
# and projects are kept in /var/lib/lmms-collab.
set -euo pipefail

SETTINGS=/etc/default/lmms-collab-server
LISTEN=0.0.0.0
PORT=42871
P4ARGS=""
if [[ -f $SETTINGS ]]; then source "$SETTINGS"; fi
P4PORT_ARG="" P4USER_ARG="" P4DEPOT_ARG=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--listen) LISTEN="$2"; shift 2 ;;
		--port) PORT="$2"; shift 2 ;;
		--p4port) P4PORT_ARG="$2"; shift 2 ;;
		--p4user) P4USER_ARG="$2"; shift 2 ;;
		--p4depot) P4DEPOT_ARG="$2"; shift 2 ;;
		--no-p4) P4ARGS=""; shift ;;
		*) echo "Unknown option $1"; exit 1 ;;
	esac
done
if [[ -n $P4PORT_ARG ]]; then
	if [[ -z $P4USER_ARG || -z $P4DEPOT_ARG ]]; then echo "--p4port needs --p4user and --p4depot"; exit 1; fi
	P4ARGS="--p4port $P4PORT_ARG --p4user $P4USER_ARG --p4depot $P4DEPOT_ARG"
fi

if [[ $EUID -ne 0 ]]; then echo "Run it with sudo."; exit 1; fi
SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # the collab folder

echo "== Installing what is needed to build it (Qt 6, CMake, a C++ compiler)"
apt-get update -q
apt-get install -y -q build-essential cmake qt6-base-dev

echo "== Building"
BUILD="$(mktemp -d)"
cmake -S "$SOURCE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel
install -m 755 "$BUILD/lmms-collab-server" /usr/local/bin/lmms-collab-server
rm -rf "$BUILD"

echo "== Service"
id lmms-collab >/dev/null 2>&1 || useradd --system --home-dir /var/lib/lmms-collab --shell /usr/sbin/nologin lmms-collab
install -d -o lmms-collab -g lmms-collab -m 750 /var/lib/lmms-collab
printf 'LISTEN=%s\nPORT=%s\nP4ARGS="%s"\n' "$LISTEN" "$PORT" "$P4ARGS" > "$SETTINGS"
install -m 644 "$SOURCE/deploy/lmms-collab-server.service" /etc/systemd/system/lmms-collab-server.service
systemctl daemon-reload
systemctl enable lmms-collab-server
systemctl restart lmms-collab-server
sleep 1
systemctl --no-pager --lines=5 status lmms-collab-server || true

echo
echo "Done: listening on $LISTEN:$PORT."
if [[ -n $P4ARGS ]]; then echo "  Versions also go to Perforce: $P4ARGS"; fi
echo "  Logs:    journalctl -u lmms-collab-server -f"
echo "  Stop:    sudo systemctl stop lmms-collab-server"
echo "  Projects (back them up!): /var/lib/lmms-collab"
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
	echo "  The firewall (ufw) is active: allow the port with  sudo ufw allow $PORT/tcp"
	echo "  (with Tailscale only:  sudo ufw allow in on tailscale0 to any port $PORT proto tcp)"
fi

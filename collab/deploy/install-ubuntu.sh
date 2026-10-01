#!/usr/bin/env bash
# Installs the LMMS collaboration server on Ubuntu (24.04 or newer) / Debian (12 or newer) as a service that starts with
# the computer. Run it from the LMMS source folder:
#
#   sudo bash collab/deploy/install-ubuntu.sh                 # listens on every address
#   sudo bash collab/deploy/install-ubuntu.sh --listen 100.x.y.z   # only on one address (e.g. your Tailscale IP)
#
# Running it again updates the server (projects are kept in /var/lib/lmms-collab).
set -euo pipefail

LISTEN=0.0.0.0
PORT=42871
while [[ $# -gt 0 ]]; do
	case "$1" in
		--listen) LISTEN="$2"; shift 2 ;;
		--port) PORT="$2"; shift 2 ;;
		*) echo "Unknown option $1"; exit 1 ;;
	esac
done

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
printf 'LISTEN=%s\nPORT=%s\n' "$LISTEN" "$PORT" > /etc/default/lmms-collab-server
install -m 644 "$SOURCE/deploy/lmms-collab-server.service" /etc/systemd/system/lmms-collab-server.service
systemctl daemon-reload
systemctl enable lmms-collab-server
systemctl restart lmms-collab-server
sleep 1
systemctl --no-pager --lines=5 status lmms-collab-server || true

echo
echo "Done: listening on $LISTEN:$PORT."
echo "  Logs:    journalctl -u lmms-collab-server -f"
echo "  Stop:    sudo systemctl stop lmms-collab-server"
echo "  Projects (back them up!): /var/lib/lmms-collab"
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
	echo "  The firewall (ufw) is active: allow the port with  sudo ufw allow $PORT/tcp"
	echo "  (with Tailscale only:  sudo ufw allow in on tailscale0 to any port $PORT proto tcp)"
fi

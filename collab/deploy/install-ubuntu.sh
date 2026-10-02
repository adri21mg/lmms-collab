#!/usr/bin/env bash
# Installs the LMMS collaboration server on Ubuntu (24.04 or newer) / Debian (12 or newer) as a service that starts with
# the computer. Run it from the LMMS source folder:
#
#   sudo bash collab/deploy/install-ubuntu.sh                      # listens on every address
#   sudo bash collab/deploy/install-ubuntu.sh --listen 100.x.y.z   # only on one address (e.g. your Tailscale IP)
#
# Security: connections are encrypted (TLS) with a certificate made here the first time, and the first install
# asks for a password everybody will need to connect (empty for none). Later:
#   --password      set or change the password        --no-password   remove it
#   --no-tls        do not encrypt (not recommended)
#
# Versions are always kept by the server. To also submit them to a Perforce (Helix Core) server, add:
#   --p4port 127.0.0.1:1666 --p4user lmms-collab --p4depot //depot/music
# (that user logs in once on this computer, see collab/deploy/README.md)
#
# Running it again updates the server; the settings of the last install are kept unless given again,
# and projects are kept in /var/lib/lmms-collab.
set -euo pipefail

SETTINGS=/etc/default/lmms-collab-server
CONF=/etc/lmms-collab
LISTEN=0.0.0.0
PORT=42871
P4ARGS=""
TLS=yes
if [[ -f $SETTINGS ]]; then source "$SETTINGS"; fi
P4PORT_ARG="" P4USER_ARG="" P4DEPOT_ARG="" PASSWORD_ACTION=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--listen) LISTEN="$2"; shift 2 ;;
		--port) PORT="$2"; shift 2 ;;
		--p4port) P4PORT_ARG="$2"; shift 2 ;;
		--p4user) P4USER_ARG="$2"; shift 2 ;;
		--p4depot) P4DEPOT_ARG="$2"; shift 2 ;;
		--no-p4) P4ARGS=""; shift ;;
		--password) PASSWORD_ACTION=set; shift ;;
		--no-password) PASSWORD_ACTION=remove; shift ;;
		--no-tls) TLS=no; shift ;;
		--tls) TLS=yes; shift ;;
		*) echo "Unknown option $1"; exit 1 ;;
	esac
done
if [[ -n $P4PORT_ARG ]]; then
	if [[ -z $P4USER_ARG || -z $P4DEPOT_ARG ]]; then echo "--p4port needs --p4user and --p4depot"; exit 1; fi
	P4ARGS="--p4port $P4PORT_ARG --p4user $P4USER_ARG --p4depot $P4DEPOT_ARG"
fi

if [[ $EUID -ne 0 ]]; then echo "Run it with sudo."; exit 1; fi
SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # the collab folder

echo "== Installing what is needed to build it (Qt 6, CMake, a C++ compiler, OpenSSL)"
apt-get update -q
apt-get install -y -q build-essential cmake qt6-base-dev openssl

echo "== Building"
BUILD="$(mktemp -d)"
cmake -S "$SOURCE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel
install -m 755 "$BUILD/lmms-collab-server" /usr/local/bin/lmms-collab-server
rm -rf "$BUILD"

echo "== Service"
id lmms-collab >/dev/null 2>&1 || useradd --system --home-dir /var/lib/lmms-collab --shell /usr/sbin/nologin lmms-collab
install -d -o lmms-collab -g lmms-collab -m 750 /var/lib/lmms-collab
install -d -o root -g lmms-collab -m 750 "$CONF"

# Password: asked the first time (and with --password); the server can read it, nobody else
if [[ $PASSWORD_ACTION == remove ]]; then rm -f "$CONF/password"; fi
if [[ $PASSWORD_ACTION == set || ( -z $PASSWORD_ACTION && ! -f $CONF/password && ! -f $CONF/no-password ) ]]; then
	while true; do
		read -r -s -p "Password everybody will need to connect (empty for none): " PASS1; echo
		if [[ -z $PASS1 ]]; then break; fi
		read -r -s -p "The same password again: " PASS2; echo
		if [[ $PASS1 == "$PASS2" ]]; then break; fi
		echo "They are not the same, again please."
	done
	if [[ -n $PASS1 ]]; then
		( umask 027; printf '%s\n' "$PASS1" > "$CONF/password" )
		rm -f "$CONF/no-password"
	else
		rm -f "$CONF/password"
		touch "$CONF/no-password" # asked once: not again on updates
	fi
	unset PASS1 PASS2
fi
if [[ -f $CONF/password ]]; then chown root:lmms-collab "$CONF/password"; chmod 640 "$CONF/password"; fi

# Encryption: a certificate of its own, made once (LMMS remembers it the first time it connects)
if [[ $TLS == yes && ! -f $CONF/tls.crt ]]; then
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 36500 \
		-subj "/CN=lmms-collab-server" -keyout "$CONF/tls.key" -out "$CONF/tls.crt" 2>/dev/null
fi
if [[ -f $CONF/tls.key ]]; then chown root:lmms-collab "$CONF/tls.key" "$CONF/tls.crt"; chmod 640 "$CONF/tls.key"; fi

SECARGS=""
if [[ -f $CONF/password ]]; then SECARGS="--password-file $CONF/password"; fi
if [[ $TLS == yes ]]; then SECARGS="$SECARGS --tls-cert $CONF/tls.crt --tls-key $CONF/tls.key"; fi
printf 'LISTEN=%s\nPORT=%s\nP4ARGS="%s"\nTLS=%s\nSECARGS="%s"\n' "$LISTEN" "$PORT" "$P4ARGS" "$TLS" "$SECARGS" > "$SETTINGS"
install -m 644 "$SOURCE/deploy/lmms-collab-server.service" /etc/systemd/system/lmms-collab-server.service
systemctl daemon-reload
systemctl enable lmms-collab-server
systemctl restart lmms-collab-server
sleep 1
systemctl --no-pager --lines=5 status lmms-collab-server || true

echo
echo "Done: listening on $LISTEN:$PORT."
if [[ -f $CONF/password ]]; then echo "  Password: yes (change it with --password)"; else echo "  Password: none (anyone who reaches the server can join; set one with --password)"; fi
if [[ $TLS == yes ]]; then
	echo "  Encrypted: yes. Certificate $(openssl x509 -noout -fingerprint -sha256 -in "$CONF/tls.crt" | cut -d= -f2)"
	echo "    (LMMS shows the same in Collaboration > Connect... the first time it connects)"
else
	echo "  Encrypted: no"
fi
if [[ -n $P4ARGS ]]; then echo "  Versions also go to Perforce: $P4ARGS"; fi
echo "  Logs:    journalctl -u lmms-collab-server -f"
echo "  Stop:    sudo systemctl stop lmms-collab-server"
echo "  Projects (back them up!): /var/lib/lmms-collab"
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
	echo "  The firewall (ufw) is active: allow the port with  sudo ufw allow $PORT/tcp"
	echo "  (with Tailscale only:  sudo ufw allow in on tailscale0 to any port $PORT proto tcp)"
fi

# Collaboration server: how to run it

Everyone who works on a shared project connects to the same **collaboration server**. The server keeps the projects
and their files, and tells each person what the others change. Pick whichever way below fits you best:

| Way | Good for | Runs while... |
|---|---|---|
| [A. Host from LMMS](#a-host-a-session-from-lmms-easiest) | trying it, a session with a friend tonight | your LMMS is open |
| [B. Windows](#b-a-server-on-windows) | a PC that is often on | its window is open |
| [C. Ubuntu / Debian server](#c-a-server-on-ubuntu-or-debian-always-on) | an always-on machine (home server, VPS) | always (starts with the computer) |

Then, so others can **reach** the server, choose [how they connect](#how-others-reach-the-server): Tailscale
(recommended), your local network, or opening a port on your router.

> **Security, please read.** For now there are no passwords and the connection is not encrypted (both are planned).
> Anyone who can reach the server can open, change and download the projects on it. That is why we recommend
> **Tailscale**: only the devices you invite can reach the server at all.

The server uses **TCP port 42871** unless you choose another one.

---

## A. Host a session from LMMS (easiest)

1. *Collaboration → Connect...* → **Host a session**.
2. The dialog shows the addresses others can use (your Tailscale address first, if you have one). Send one of them
   to your friends; they put it in *Server:* in their own *Connect...* dialog.
3. Share your song (*Share my current song as a new project...*) or join a project that is already there.

The server stops when you close LMMS or choose *Collaboration → Stop hosting the session*. Projects are kept in the
`collab-host` folder of your LMMS working directory, so you can host them again later.

Windows asks the first time whether `lmms-collab-server` may use the network: allow it on **private networks**.

## B. A server on Windows

1. Double-click `start-collab-server.bat` in the LMMS folder (next to `lmms.exe`). A black window opens: the server
   runs while it stays open.
2. Projects are stored in `collab-server-data` next to it. Close the window (or press Ctrl+C) to stop the server;
   it saves everything first.
3. If Windows asks about network access, allow it on **private networks**. If you never got the question:
   *Windows Security → Firewall & network protection → Allow an app through firewall → Allow another app* →
   `lmms-collab-server.exe`.

To start it with Windows: press Win+R, type `shell:startup`, and put a shortcut to `start-collab-server.bat` there.

Want another port or folder? Edit the `.bat` with Notepad: `--port 42871` and `--data "..."`.

## C. A server on Ubuntu or Debian (always on)

On the server, with the LMMS source code (the `collab` folder is enough):

```bash
git clone https://github.com/adri21mg/lmms-collab.git
cd lmms-collab
sudo bash collab/deploy/install-ubuntu.sh
```

The script installs what is needed to build it (Qt 6, CMake, a compiler), builds only the server, and installs it
as a service (`lmms-collab-server`) that starts with the computer and restarts if it ever crashes. It runs as its own
user, `lmms-collab`, which can only write to `/var/lib/lmms-collab`, where the projects are kept.

To accept connections **only through Tailscale**, give it your Tailscale address (`tailscale ip -4`):

```bash
sudo bash collab/deploy/install-ubuntu.sh --listen 100.x.y.z
```

Useful commands:

```bash
sudo systemctl status lmms-collab-server     # is it running?
journalctl -u lmms-collab-server -f          # what is it doing (who joins, saves, errors)
sudo systemctl restart lmms-collab-server    # restart it (it saves first)
```

**Updating:** `git pull` in the source folder and run the install script again. Projects are kept.

Settings (address and port) are in `/etc/default/lmms-collab-server`; after changing them run
`sudo systemctl restart lmms-collab-server`.

Other Linux distributions work too: build it with `cmake -S collab -B build -DCMAKE_BUILD_TYPE=Release` and
`cmake --build build` (needs Qt 6 Base and CMake), then run `build/lmms-collab-server --listen 0.0.0.0 --data <folder>`.

---

## How others reach the server

### Tailscale (recommended)

[Tailscale](https://tailscale.com) makes a private network between your devices, wherever they are, without opening
anything on your router. It is free for personal use.

1. Install Tailscale on the server computer and on each collaborator's computer, and sign in.
2. Share the server with your friends: in the Tailscale admin page, *Machines → (the server) → Share...*, or invite
   them to your tailnet.
3. They connect to `100.x.y.z:42871` (the server's Tailscale address; `tailscale ip -4` shows it, and the
   *Host a session* dialog lists it first).

Nobody outside your Tailscale network can even see the server.

### Same house / local network

Everyone on the same Wi-Fi or LAN can connect to the server's local address, like `192.168.1.20:42871`
(*Host a session* lists it; on Linux `hostname -I`, on Windows `ipconfig`).

### Opening a port on your router (port forwarding)

Only if Tailscale is not an option. Remember: **anyone on the internet** who finds the address can then join.

1. Give the server computer a fixed local address (a "DHCP reservation" in your router).
2. In the router: *Port forwarding* (sometimes *NAT* or *Virtual server*) → forward **TCP 42871** to that address.
3. Allow the port in the server's firewall:
   - Ubuntu with ufw: `sudo ufw allow 42871/tcp`
   - Windows: allow the app as in [B](#b-a-server-on-windows), on public networks too.
4. Collaborators connect to your public address (search "what is my IP"), e.g. `203.0.113.7:42871`. If it changes
   often, a free dynamic DNS name (DuckDNS, No-IP...) helps.

Some internet providers (CG-NAT) do not allow port forwarding at all: Tailscale works anyway.

---

## Versions (and Perforce, optional)

*Collaboration → Create version...* keeps the project as it is now, with a description, so you can go back to it
later (*Collaboration → Versions...* lists them). Versions are made only when someone asks for one, never
automatically. The server keeps them in the `versions` folder of each project: nothing to set up.

If you use **Perforce (Helix Core)**, the server can also submit every version there, as a changelist with the
description. Only the server talks to Perforce: collaborators need no Perforce setup at all. In the depot:

```
//depot/music/.gdignore                        (an empty file: Godot ignores the folder)
//depot/music/<project>/<project>.mmp          (opens in LMMS directly)
//depot/music/<project>/Project files/...      (samples and other shared files)
```

Setup, once:

1. Create a Perforce user for the server (e.g. `lmms-collab`) with write access to the music folder only
   (`write user lmms-collab * //depot/music/...` in `p4 protect`), and put it in a group with
   `Timeout: unlimited` so its login never expires.
2. Log it in once on the server computer, as the service user:
   ```bash
   sudo -u lmms-collab env HOME=/var/lib/lmms-collab p4 -p 127.0.0.1:1666 -u lmms-collab login
   ```
   (the `p4` command-line client must be installed; use your server's address and port)
3. Install (or update) the server with the Perforce settings:
   ```bash
   sudo bash collab/deploy/install-ubuntu.sh --p4port 127.0.0.1:1666 --p4user lmms-collab --p4depot //depot/music
   ```

The server creates its own workspace (`lmms-collab_<computer name>`, mapping only the music folder). If Perforce is
unreachable, the version is still kept on the server and whoever created it is told why it was not submitted.
On Windows, add the same `--p4port`, `--p4user` and `--p4depot` options to `start-collab-server.bat`.

## Backups

All projects and their files are in one folder: `/var/lib/lmms-collab` (Ubuntu service), `collab-server-data`
(Windows `.bat`) or `collab-host` (hosting from LMMS). Copy that folder now and then; for a consistent copy, stop the
server first. To restore, put the folder back and start the server.

Every person can also keep their own copy at any time with *File → Save As...* in LMMS.

## If something goes wrong

- **"Cannot reach the server"**: is the server running? Same port on both sides? With Tailscale, is it connected on
  both computers? Is the firewall allowing it?
- **The connection drops**: LMMS keeps trying to reconnect by itself. If you changed something while offline, it
  offers to save your version as a separate file when it is back.
- The server log (`journalctl -u lmms-collab-server`, or the black window on Windows) shows who joins and why
  connections fail.

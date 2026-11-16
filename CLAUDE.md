# js8mesh: JS8Call ↔ Meshtastic gateway

A Python gateway that lets an allowed Meshtastic node use JS8Call over DMs: send JS8 messages, read the inbox, check heard stations and status, and set the frequency and offset. Received JS8 traffic for WM8Q and @groups is forwarded to the mesh. The command list is in `COMMANDS.md`; keep it in sync when commands change.

## Files

- `js8mesh.py`: the gateway. Settings (node name, allowed nodes, limits) are constants at the top.
- `run-js8mesh.sh`: wrapper that restarts the gateway when it exits. It logs to `js8mesh.log` (gitignored).

## Running

- Python environment: `~/meshtastic-venv` (meshtastic 2.7.11, pyjs8call 0.2.3). Run with `~/meshtastic-venv/bin/python`, not the system python.
- Start: `~/js8mesh/run-js8mesh.sh`. JS8Call should be running first, though pyjs8call will start it if not.
- Stop: kill by PID. `pgrep -f '^/bin/bash /home/john/js8mesh/run-js8mesh.sh'` finds the wrapper and `pgrep -f '^/home/john/meshtastic-venv/bin/python js8mesh.py'` finds the gateway. Never run `pkill -f js8mesh` from a shell command: the pattern matches the command itself and kills it.
- Test offline by importing `js8mesh`, swapping `js8mesh.js8` for a fake, calling `handle_command(...)` and reading `js8mesh.mesh_out`. This doesn't touch either radio.

## Station

- Callsign WM8Q, grid DN61. Radio: Icom IC-7300, CAT and PTT on `/dev/ttyUSB0` (user is in `dialout`).
- JS8Call 2.2.0 with the TCP API on 127.0.0.1:2442. Config file: `~/.config/JS8Call.ini`. Known-good backup: `JS8Call.ini.good-2026-11-15`.
- Mesh nodes: Rpt1 `!0b7bbe91` (BLE `FD:08:0B:7B:BE:91`, PIN 123456, ROUTER role; the gateway connects to it), Wio1 `!4b7e12ad` (the user's handheld, the only allowed node), Rpt2 `!63619e66`, a5bd `!977aa5bd`.

## Hazards (learned the hard way)

- **pyjs8call rewrites `JS8Call.ini`** and once emptied it while JS8Call was running. The gateway disables this with `js8.config.write = lambda: None`. Keep that line.
- **pyjs8call's exit handler closes JS8Call.** The gateway re-registers it with `terminate_js8call=False`. Keep that too.
- **pyjs8call auto-offset** (OffsetMonitor) moves the TX offset by itself within 1000–2500 Hz. It is re-enabled on every start.
- **Bluetooth to Rpt1 is marginal** (about −70 to −95 dBm). Connect by name `Rpt1_be91`, not by address, which timed out. A stale BLE connection left by a killed gateway stops Rpt1 from advertising, so the wrapper runs `bluetoothctl disconnect` before each start. The Meshtastic CLI over BLE often fails once before it works, and hangs on exit after success, so run it under `timeout`.
- **JS8 end-of-message mark** (♢) arrives garbled as `â\x99`. The gateway strips non-ASCII from forwarded text.
- **apt is blocked** because JS8Call is half-installed: it needs `wsjtx-data`, which clashes with the user's FT8-GO package over `wsjtx_icon.png`. Install packages with `apt-get download` and `sudo dpkg -i`, after checking for file conflicts.
- **Low-RAM PC** (1.8 GB). Use `nice -n 19`, single-job builds.

## Working with the user

- Keep replies short.
- Don't block on multi-minute waits. Take a quick snapshot of the state, report it, and let the user say when to check again.
- Anything that transmits on HF (SEND, HB) needs the user's go-ahead before it's tested live.

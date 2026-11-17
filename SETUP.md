# Setting up the gateway

This guide sets up the gateway on a Windows or Linux PC. When you're done, you can send JS8 messages and commands from a Meshtastic handheld, and JS8 traffic for your callsign arrives on the handheld as direct messages (DMs).

## How it fits together

```
handheld  --LoRa-->  gateway node  --Bluetooth LE-->  gateway PC  --WiFi-->  radio PC (Subspace)  --USB-->  radio
```

- **Handheld:** your Meshtastic device, for example a Wio Tracker L1. You type commands on it.
- **Gateway node:** a second Meshtastic node, for example one used as a router. The gateway PC connects to it over Bluetooth LE.
- **Gateway PC:** any PC with Bluetooth LE (Bluetooth 4.0 or newer) that runs this program. An old laptop is enough.
- **Radio PC:** the PC that runs Subspace and controls the radio.

The gateway PC and the radio PC can be the same machine.

## What you need

- A radio PC running [Subspace Edition](https://github.com/avantol/Subspace-Edition/releases/latest), set up and working with your radio.
- Two Meshtastic devices: the handheld and the gateway node. They need the same LoRa settings so they can reach each other.
- A gateway PC with Bluetooth LE, on the same home network as the radio PC, within Bluetooth range of the gateway node.
- Python 3.10 or newer on the gateway PC.

## Step 1: Turn on Subspace's phone remote (radio PC)

1. In Subspace, open **File > Phone remote**.
2. Check **Allow a phone to operate this station**.
3. Leave **Port** at 2443, unless something else already uses that port.
4. Write down the **Pass code**, which you can change with **New code**, and the **Home network** address, for example `http://192.168.1.50:2443`.
5. On Windows, if a firewall prompt appears, allow Subspace on **private** networks.

Give the radio PC a reserved (fixed) IP address in your router. Otherwise the address can change and the gateway can no longer find Subspace.

The phone remote allows one login at a time. When the gateway is running, logging in from a phone logs the gateway out. The gateway logs back in after 2 minutes, which then logs the phone out.

## Step 2: Get the Meshtastic details

You need three things:

| What | Where to find it | Example |
|---|---|---|
| The gateway node's **Bluetooth name** | Run `meshtastic --ble-scan` on the gateway PC (after Step 3), or look for it in the Meshtastic app's Bluetooth list | `Rpt1_be91` |
| The gateway node's **Bluetooth PIN** | The node's screen, or the app (Settings > Bluetooth). The default fixed PIN is `123456`. | `123456` |
| The handheld's **node ID** | Meshtastic app > node list > your handheld | `!8a31cb4a` |

A Meshtastic node accepts only one Bluetooth connection at a time. While the gateway is connected, your phone app can't connect to the gateway node, but it can still connect to your handheld.

## Step 3: Install the gateway (gateway PC)

Download this repository as a ZIP from GitHub (**Code > Download ZIP**) and unzip it, or clone it with git.

### Linux

```bash
sudo apt install python3-venv bluez        # Debian, Ubuntu, Mint
python3 -m venv ~/meshtastic-venv
~/meshtastic-venv/bin/pip install meshtastic==2.7.11 pyjs8call==0.2.3 requests
```

Pair the gateway node once, using the Bluetooth name and PIN from Step 2:

```bash
bluetoothctl
  scan on              # wait until the node's name appears, and note its address (XX:XX:...)
  pair XX:XX:XX:XX:XX:XX
  trust XX:XX:XX:XX:XX:XX
  disconnect XX:XX:XX:XX:XX:XX
  quit
```

### Windows

1. Install Python from [python.org](https://www.python.org/downloads/windows/). In the installer, check **Add python.exe to PATH**.
2. Open **Command Prompt** and run:

   ```bat
   py -m venv %USERPROFILE%\meshtastic-venv
   %USERPROFILE%\meshtastic-venv\Scripts\pip install meshtastic==2.7.11 pyjs8call==0.2.3 requests
   ```

3. Pair the gateway node in **Settings > Bluetooth & devices > Add device > Bluetooth**, entering the PIN from Step 2 when asked.

## Step 4: Configure the gateway

Open `js8mesh.py` in a text editor. Near the top, under `settings`, change these lines:

```python
MESH_NODE = "Rpt1_be91"                    # the gateway node's Bluetooth name (Step 2)
ALLOWED_NODES = {"!8a31cb4a"}              # your handheld's node ID; list more, comma-separated
SUBSPACE_URL = "http://10.21.1.202:2443"   # Subspace's Home network address (Step 1)
JS8_BACKEND = os.environ.get("JS8MESH_BACKEND", "subspace")   # "subspace" or "local"
```

Only nodes in `ALLOWED_NODES` can send commands, and JS8 messages are forwarded only to them.

Save the Subspace pass code in a file of its own, outside the repository:

**Linux**

```bash
mkdir -p ~/.config/js8mesh
echo 'YOURPASSCODE' > ~/.config/js8mesh/subspace-passcode
chmod 600 ~/.config/js8mesh/subspace-passcode
```

**Windows (PowerShell)**

```powershell
New-Item -ItemType Directory -Force "$HOME\.config\js8mesh" | Out-Null
Set-Content "$HOME\.config\js8mesh\subspace-passcode" "YOURPASSCODE"
```

## Step 5: Run it

### Linux

Run it once in a terminal to check that it works:

```bash
cd /path/to/Subspace-Meshtastic-Basic
~/meshtastic-venv/bin/python js8mesh.py
```

For everyday use, run `run-js8mesh.sh`. It restarts the gateway whenever the program exits, for example when the Bluetooth link drops. First edit the script:

- Change the address on the `bluetoothctl disconnect` line to your gateway node's address.
- If you put the venv somewhere else, change the `~/meshtastic-venv/bin/python` path.

Then start it so it keeps running after you close the terminal:

```bash
setsid nohup ./run-js8mesh.sh >/dev/null 2>&1 < /dev/null &
```

Output goes to `js8mesh.log` in the same folder.

### Windows

Run it once in Command Prompt to check that it works:

```bat
cd C:\path\to\Subspace-Meshtastic-Basic
%USERPROFILE%\meshtastic-venv\Scripts\python js8mesh.py
```

For everyday use, save the following as `run-js8mesh.bat` in the same folder and double-click it. It restarts the gateway whenever the program exits, and its output goes to `js8mesh.log`.

```bat
@echo off
cd /d %~dp0
:loop
"%USERPROFILE%\meshtastic-venv\Scripts\python.exe" js8mesh.py >> js8mesh.log 2>&1
echo %date% %time% gateway exited; restarting in 15 s >> js8mesh.log
timeout /t 15 /nobreak >nul
goto loop
```

### Check it

A good start looks like this:

```
Connecting to Subspace at http://192.168.1.50:2443 over WiFi...
Subspace connected as N0CALL
Connecting to Meshtastic node Rpt1_be91 over Bluetooth...
Meshtastic connected. Allowed nodes: !8a31cb4a
```

From your handheld, send a **direct message** (not a channel message) containing `pi` to the gateway node. The reply should be `Gateway up. Subspace (WiFi) connected as N0CALL.` Send `h` for the command list. All commands are described in [COMMANDS.md](COMMANDS.md).

## Troubleshooting

| Problem | What to check |
|---|---|
| `Bluetooth connection failed`, or it hangs at `Connecting to Meshtastic node` | Is the node in range and powered on? Is the phone app connected to it? Disconnect the app. Is `MESH_NODE` exactly the Bluetooth name? On Linux, a link left over from a killed run stops the node from advertising; run `bluetoothctl disconnect <address>`. |
| Bluetooth keeps dropping | The signal is weak. Move the gateway PC or the node closer together. The restart script reconnects on its own. |
| `Subspace connection failed: wrong pass code` | Check the pass code file. Five wrong tries lock logins for 60 seconds. |
| `Connection refused` or a timeout reaching Subspace | Is Subspace running with the phone remote turned on? Is `SUBSPACE_URL` the Home network address? On Windows, check the firewall on the radio PC. |
| `Subspace: session ended (another login...)` in the log | A phone logged in to the Subspace remote. The gateway takes the session back after 2 minutes. |
| The handheld gets no reply | Is it a direct message to the gateway node? Is the handheld's ID in `ALLOWED_NODES`? The log shows `ignored: sender not in ALLOWED_NODES` when it isn't. |
| A message is queued but never transmitted | Is TX turned on at the station? The reply to SEND, HB, CQ, REPLY and SNR warns when TX is off. |

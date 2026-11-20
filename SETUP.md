# Setting up the gateway

This guide sets up the JS8 Mesh Gateway app on a Windows or Linux PC. When you're done, you can send JS8 messages and commands from a Meshtastic handheld, and JS8 traffic for your callsign arrives on the handheld as direct messages (DMs).

## How it fits together

```
handheld  --LoRa-->  gateway node  --Bluetooth LE-->  gateway PC  --WiFi-->  radio PC (Subspace)  --USB-->  radio
```

- **Handheld:** your Meshtastic device, for example a Wio Tracker L1. You type commands on it.
- **Gateway node:** a second Meshtastic node, for example one used as a router. The gateway PC connects to it over Bluetooth LE (or USB, or WiFi if the node has it).
- **Gateway PC:** any PC with Bluetooth LE (Bluetooth 4.0 or newer) that runs the gateway app. An old laptop is enough.
- **Radio PC:** the PC that runs Subspace and controls the radio.

The gateway PC and the radio PC can be the same machine.

## What you need

- A radio PC running [Subspace Edition](https://github.com/avantol/Subspace-Edition/releases/latest), set up and working with your radio.
- Two Meshtastic devices: the handheld and the gateway node. They need the same LoRa settings so they can reach each other.
- A gateway PC on the same home network as the radio PC, within Bluetooth range of the gateway node, running one of:
  - Windows 10 or 11 (64-bit or 32-bit)
  - Linux Mint 22 (or Ubuntu 24.04)

## Step 1: Turn on Subspace's phone remote (radio PC)

1. In Subspace, open **File > Phone remote**.
2. Check **Allow a phone to operate this station**.
3. Leave **Port** at 2443, unless something else already uses that port.
4. Write down the **Pass code**, which you can change with **New code**, and the **Home network** address, for example `http://192.168.1.50:2443`.
5. On Windows, if a firewall prompt appears, allow Subspace on **private** networks.

Give the radio PC a reserved (fixed) IP address in your router. Otherwise the address can change and the gateway can no longer find Subspace.

The phone remote allows one login at a time. When the gateway is running, logging in from a phone logs the gateway out. The gateway logs back in after 2 minutes (you can change this), which then logs the phone out.

## Step 2: Get the Meshtastic details

| What | Where to find it | Example |
|---|---|---|
| The gateway node's **Bluetooth PIN** | The node's screen, or the Meshtastic app (Settings > Bluetooth). The usual fixed PIN is `123456`. | `123456` |
| The handheld's **node ID** | Meshtastic app > node list > your handheld | `!8a31cb4a` |

The gateway node's Bluetooth name (like `Rpt1_be91`) you can pick from a list in the app later.

A Meshtastic node accepts only one Bluetooth connection at a time. While the gateway is connected, your phone app can't connect to the gateway node, but it can still connect to your handheld.

## Step 3: Install the app (gateway PC)

Download it from the [latest release](https://github.com/avantol/Subspace-Meshtastic-Basic/releases/latest).

### Windows

1. Download the setup file for your Windows:
   - 64-bit (most PCs): `js8mesh-<version>-windows-64bit-setup.exe`
   - 32-bit: `js8mesh-<version>-windows-32bit-setup.exe`

   Not sure which? **Settings > System > About > System type**.
2. Run it. Windows may warn that the app is from an unknown publisher: click **More info > Run anyway**.
3. In the installer, you can choose **Start the gateway when Windows starts**.

If you'd rather not install anything, the `…-portable.zip` files hold the same app: unzip and run `js8mesh.exe`.

**Pair the gateway node:** **Settings > Bluetooth & devices > Add device > Bluetooth**, pick the node, and enter its PIN from Step 2.

### Linux Mint

1. Download `js8mesh-<version>-linuxmint22-amd64.deb`.
2. Install it (this also installs what it needs):

   ```bash
   sudo apt install ./js8mesh-*-linuxmint22-amd64.deb
   ```

3. **Pair the gateway node** once, with the PIN from Step 2:

   ```bash
   bluetoothctl
     scan on              # wait until the node's name appears, and note its address (XX:XX:...)
     pair XX:XX:XX:XX:XX:XX
     trust XX:XX:XX:XX:XX:XX
     disconnect XX:XX:XX:XX:XX:XX
     quit
   ```

The app is in the menu as **JS8 Mesh Gateway** (or run `js8mesh`).

## Step 4: Set up the app

Open **JS8 Mesh Gateway**. Fill in the tabs, then click **Save settings**.

**Meshtastic node**
- **Connect by:** Bluetooth LE (or USB serial / TCP if your node is plugged in or on WiFi).
- **Node's Bluetooth name or address:** click **Scan...**, pick the gateway node (Meshtastic nodes are listed first), and click OK.
- **Allowed nodes:** your handheld's node ID from Step 2, with the `!`. Only these nodes can send commands, and JS8 messages are forwarded only to them.

**Subspace station**
- **Address (Home network):** from Step 1, e.g. `http://192.168.1.50:2443`.
- **Pass code:** from Step 1.

**Forwarding & limits:** which JS8 @groups to forward to the handheld (none by default; messages to your callsign are always forwarded), and limits like the longest mesh message.

**Options**
- **Start the gateway when this app opens.**
- **Closing the window keeps the gateway running in the system tray.**

To start the gateway with your PC on Linux, add `js8mesh --start --minimized` to **Startup Applications**. On Windows, use the installer's startup option.

## Step 5: Start it and check

Click **Start gateway** on the **Gateway** tab. A good start in the log looks like this:

```
Subspace: logged in at http://192.168.1.50:2443
JS8 station connected as N0CALL
Found Rpt1_be91 (FD:08:0B:7B:BE:91, -70 dBm); connecting...
Meshtastic connected (Bluetooth Rpt1_be91, node !0b7bbe91). Allowed nodes: !8a31cb4a
```

The Bluetooth step can take up to a minute when the signal is weak.

From your handheld, send a **direct message** (not a channel message) containing `pi` to the gateway node. The log shows `MESH RX <- !8a31cb4a … 'pi'`, and the handheld gets `Gateway up. … connected as N0CALL.` Send `h` for the command list. All commands are described in [COMMANDS.md](COMMANDS.md).

The log is also saved to a file; its path is shown above the log on the Gateway tab.

## Troubleshooting

| Problem | What to check |
|---|---|
| `Bluetooth device '…' not found … Seen: …` | Is the node in range and powered on? Is the phone app, or another gateway, connected to it? Disconnect it. Is your node in the `Seen:` list? If so, pick it again with **Scan...** and save. |
| `Bluetooth: … failed. The node probably needs pairing` | Pair the node with this PC (Step 3), then start again. |
| `Bluetooth connection timed out (weak signal?)` or the link keeps dropping | Move the gateway PC and the node closer together. The gateway reconnects on its own. |
| `Subspace: wrong pass code` | Check the pass code. Five wrong tries lock logins for 60 seconds. |
| `Subspace: no answer from …` or `… refused the connection` | Is Subspace running with the phone remote turned on? Is the address the Home network one? Can a browser on the gateway PC open it? On Windows, check the firewall on the radio PC. |
| `Subspace: session ended (another login…)` | A phone logged in to the Subspace remote. The gateway takes the session back after the wait set on the Subspace station tab. |
| The handheld gets no reply | Is it a direct message to the gateway node? The log shows `MESH channel text … ignored` for a channel message, and `ignored: sender not in the allowed nodes` when the handheld's ID isn't in **Allowed nodes**. |
| A message is queued but never transmitted | Is TX turned on at the station? The replies to SEND, HB, CQ, REPLY, SNR and AGAIN warn when TX is off. |

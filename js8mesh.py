#!/usr/bin/env python3
"""JS8Call <-> Meshtastic gateway (barebones).

Runs on this PC, connected to JS8Call (via pyjs8call) and to a Meshtastic node over
Bluetooth (via the meshtastic library). From an allowed Meshtastic node, send a direct
message (DM) to the gateway node with one of these commands:

    SEND [CALL or @GROUP] <message>   transmit a JS8 directed message as your callsign
                                      (no CALL: to the call or @group selected at the station)
    INBOX                             list the latest JS8Call inbox messages
    HEARD                             stations heard in the last hour
    STATUS                            dial, offset, speed, last RX
    FREQ [MHz]                        show, or set, the radio's dial frequency
    OFFSET [Hz|AUTO]                  show or set the TX audio offset (setting turns auto-move off)
    HB                                transmit a JS8 heartbeat
    CALL [CALL|NONE]                  show, or set (Subspace only), the selected callsign
    GROUPS [@G ...|ALL|NONE]          show or set which @groups are forwarded to the mesh
    PING                              check the gateway is up
    HELP                              list commands

Received JS8 messages addressed to your callsign or to a forwarded @group (see GROUPS;
@HB heartbeats never) are DMed to every allowed node.

JS8 backend (JS8_BACKEND below, or the JS8MESH_BACKEND environment variable):
  "local"     JS8Call on this PC (radio on local USB), through pyjs8call.
  "subspace"  a Subspace Edition station elsewhere on the WiFi, through its phone-remote
              server (subspace_remote.py). Its pass code is read from SUBSPACE_PASSCODE_FILE.

Run with ~/meshtastic-venv/bin/python, or use run-js8mesh.sh, which restarts the
gateway if the Bluetooth link drops.
"""
import atexit
import json
import os
import queue
import re
import threading
import time

import pyjs8call
import subspace_remote
from pubsub import pub
from meshtastic.ble_interface import BLEInterface

# ---- settings --------------------------------------------------------------
MESH_NODE = "Rpt1_be91"               # Rpt1 (FD:08:0B:7B:BE:91), the node this PC connects to over BLE
ALLOWED_NODES = {"!4b7e12ad"}         # Wio1. Only these nodes may use commands; RX goes to them.
INBOX_COUNT = 5                       # how many inbox messages INBOX returns
MESH_CHUNK = 200                      # max characters per Meshtastic message
MESH_GAP = 3.0                        # seconds between outgoing mesh messages (LoRa airtime)
DEDUPE_SECONDS = 600                  # ignore a repeat of the same JS8 message within this time
HEARD_SECONDS = 3600                  # HEARD lists stations heard within this time
HEARD_COUNT = 8                       # ...up to this many, most recent first
TX_REPORT_SECONDS = 300               # how long to wait for JS8Call to transmit before reporting
JS8_BACKEND = os.environ.get("JS8MESH_BACKEND", "local")   # "local" or "subspace" (see above)
SUBSPACE_URL = "http://10.21.1.202:2443"   # Subspace phone-remote server on the home WiFi
SUBSPACE_PASSCODE_FILE = os.path.expanduser("~/.config/js8mesh/subspace-passcode")
GROUPS_FILE = os.path.expanduser("~/.config/js8mesh/forward-groups.json")  # set by the GROUPS command
SUBSPACE_RELOGIN_SECONDS = 120        # after the phone logs in (ending our session), wait this long
                                      # before logging back in (which logs the phone out)
# ---------------------------------------------------------------------------

# Short forms: first two letters of each command. HE is HEARD; HELP is H or ?.
ALIASES = {"SE": "SEND", "IN": "INBOX", "HE": "HEARD", "ST": "STATUS", "FR": "FREQ",
           "OF": "OFFSET", "PI": "PING", "GR": "GROUPS", "CA": "CALL", "H": "HELP"}
OFFSET_MIN, OFFSET_MAX = 300, 2700    # audio offset range OFFSET accepts (Hz)

GROUP_RE = re.compile(r"^@[A-Z0-9]{1,8}$")
CALL_RE = re.compile(r"^(@[A-Z0-9]{1,8}|[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}(/[A-Z0-9]{1,4})?)$")

mesh_out = queue.Queue()              # (destination node id, text)
recent = {}                           # dedupe: key -> time seen
js8 = None
mesh = None


def log(*args):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *args, flush=True)


# ---- forwarded groups ---------------------------------------------------------
def load_groups():
    """The @groups to forward, or None for all groups."""
    try:
        with open(GROUPS_FILE) as f:
            groups = json.load(f)
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as e:
        log("Can't read", GROUPS_FILE, "- forwarding all groups:", e)
        return None
    return None if groups is None else set(groups)


def save_groups(groups):
    os.makedirs(os.path.dirname(GROUPS_FILE), exist_ok=True)
    tmp = GROUPS_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(None if groups is None else sorted(groups), f)
    os.replace(tmp, GROUPS_FILE)


def describe_groups(groups):
    if groups is None:
        return "Forwarding all @groups (not @HB)."
    if not groups:
        return "Forwarding no @groups (only messages to you)."
    return "Forwarding @groups: " + " ".join(sorted(groups))


forward_groups = load_groups()


# ---- Meshtastic side --------------------------------------------------------
def mesh_send(dest, text):
    """Queue text for a DM, split into LoRa-sized pieces."""
    text = text.strip()
    pieces = [text[i:i + MESH_CHUNK] for i in range(0, len(text), MESH_CHUNK)] or [""]
    for n, piece in enumerate(pieces, 1):
        if len(pieces) > 1:
            piece = f"({n}/{len(pieces)}) {piece}"
        mesh_out.put((dest, piece))


def mesh_sender():
    """Send queued DMs one at a time, spaced out to respect LoRa airtime."""
    while True:
        dest, text = mesh_out.get()
        try:
            mesh.sendText(text, destinationId=dest, wantAck=True)
            log("MESH TX ->", dest, repr(text))
        except Exception as e:  # noqa: BLE001 - keep the sender alive
            log("MESH TX failed:", e)
        time.sleep(MESH_GAP)


def on_mesh_text(packet, interface):
    try:
        my_num = interface.myInfo.my_node_num
    except AttributeError:
        return
    if packet.get("to") != my_num:
        return  # channel traffic, not a DM to the gateway
    sender = packet.get("fromId")
    text = (packet.get("decoded", {}).get("text") or "").strip()
    log("MESH RX <-", sender, repr(text))
    if sender not in ALLOWED_NODES:
        log("  ignored: sender not in ALLOWED_NODES")
        return
    threading.Thread(target=handle_command, args=(sender, text), daemon=True).start()


def on_mesh_lost(interface):
    log("Bluetooth link to the Meshtastic node was lost; exiting so the wrapper restarts.")
    os._exit(2)


# ---- commands ---------------------------------------------------------------
def js8_tx_debug():
    if hasattr(js8, "tx_debug"):
        return js8.tx_debug()
    return (f"pyjs8call queue {len(js8.js8call._tx_queue)}, "
            f"tx box {js8.js8call.get_state('tx_text')!r}")


def report_tx(sender, msg, label):
    """Follow a queued JS8 message and tell the sender whether it was transmitted."""
    def watch():
        last = None
        end = time.time() + TX_REPORT_SECONDS
        while time.time() < end:
            status = getattr(msg, "status", None)
            if status != last:
                log(f"JS8 TX status [{label}]: {status} | {js8_tx_debug()}")
                last = status
            if status == pyjs8call.Message.STATUS_SENT:
                mesh_send(sender, f"Transmitted on JS8: {label}")
                return
            if status in (pyjs8call.Message.STATUS_FAILED, pyjs8call.Message.STATUS_ERROR):
                mesh_send(sender, f"JS8 did NOT transmit ({status}): {label}")
                return
            time.sleep(2)
        mesh_send(sender, f"JS8 hasn't transmitted after {TX_REPORT_SECONDS // 60} min "
                          f"(status {last}): {label}")
    threading.Thread(target=watch, daemon=True).start()


def is_call(word):
    """A callsign or @group. A callsign needs a letter, so "73" is message text."""
    return bool(CALL_RE.match(word)) and (word.startswith("@") or any(c.isalpha() for c in word))


def tx_off_warning():
    """For the Subspace backend: a note if the station's TX button is off."""
    try:
        if hasattr(js8, "tx_enabled") and not js8.tx_enabled():
            return " WARNING: TX is OFF at the station."
    except Exception:  # noqa: BLE001
        pass
    return ""


def handle_command(sender, text):
    global forward_groups
    words = text.split(None, 2)
    cmd = words[0].upper().lstrip(".") if words else ""   # a leading "." is optional
    cmd = ALIASES.get(cmd, cmd)

    if cmd in ("HELP", "?"):
        mesh_send(sender, "JS8 gateway (short forms in caps, leading . optional): "
                          "SEnd [CALL|@GROUP] <msg> | INbox | HEard | STatus | FReq [MHz] | "
                          "OFfset [Hz|AUTO] | GRoups [@G ..|ALL|NONE] | CAll [CALL|NONE] | "
                          "HB | PIng | H/?")

    elif cmd == "PING":
        state = "connected" if js8.connected() else "NOT connected"
        where = "Subspace (WiFi)" if JS8_BACKEND == "subspace" else "JS8Call"
        mesh_send(sender, f"Gateway up. {where} {state} as {js8.settings.get_station_callsign()}.")

    elif cmd == "SEND":
        usage = "Usage: SEND [CALL|@GROUP] <message> (no CALL: the selected call, see CA)"
        if len(words) >= 3 and is_call(words[1].upper()):
            dest, body = words[1].upper(), words[2].upper()
        elif len(words) == 2 and is_call(words[1].upper()):
            mesh_send(sender, f"No message for {words[1].upper()}. " + usage)
            return
        elif len(words) >= 2:
            # no destination given: send to the call or @group selected at the station
            body = text.split(None, 1)[1].upper()
            try:
                dest = (js8.get_selected_call() or "").upper()
            except Exception as e:  # noqa: BLE001
                mesh_send(sender, f"Couldn't read the selected call: {e}")
                return
            if not dest:
                mesh_send(sender, "No call selected. " + usage)
                return
            if not is_call(dest):
                mesh_send(sender, f"Selected call '{dest}' doesn't look like a callsign or @group.")
                return
        else:
            mesh_send(sender, usage)
            return
        try:
            msg = js8.send_directed_message(dest, body)
            log("JS8 TX queued:", dest, repr(body))
            mesh_send(sender, f"Queued for JS8 TX: {dest} {body}{tx_off_warning()}")
            report_tx(sender, msg, f"{dest} {body}")
        except Exception as e:  # noqa: BLE001
            log("JS8 TX failed:", e)
            mesh_send(sender, f"JS8 send failed: {e}")

    elif cmd == "INBOX":
        try:
            msgs = js8.get_inbox_messages(unread=False) or []
        except Exception as e:  # noqa: BLE001
            mesh_send(sender, f"Inbox read failed: {e}")
            return
        if not msgs:
            mesh_send(sender, "JS8 inbox is empty.")
            return
        msgs = sorted(msgs, key=lambda m: m.get("timestamp") or 0)[-INBOX_COUNT:]
        mesh_send(sender, f"JS8 inbox, latest {len(msgs)} (* = unread):")
        for m in msgs:
            new = "*" if m.get("unread") else ""
            mesh_send(sender, f"{new}{m.get('utc_time_str', '')} {m.get('origin', '?')}>"
                              f"{m.get('destination', '?')}: {m.get('text') or m.get('value', '')}")

    elif cmd == "FREQ":
        try:
            if len(words) >= 2:
                mhz = float(words[1])
                hz = int(round(mhz * 1e6))
                if pyjs8call.Client.freq_to_band(hz) == pyjs8call.Client.OOB:
                    mesh_send(sender, f"{mhz} MHz is outside the ham bands; not changed.")
                    return
                js8.settings.set_freq(hz)
                log("JS8 dial set to", hz)
            hz = js8.settings.get_freq(update=True)
            mesh_send(sender, f"Dial {hz / 1e6:.3f} MHz ({pyjs8call.Client.freq_to_band(hz)})")
        except ValueError:
            mesh_send(sender, "Usage: FREQ  or  FREQ <MHz>, e.g. FREQ 14.078")
        except Exception as e:  # noqa: BLE001
            mesh_send(sender, f"FREQ failed: {e}")

    elif cmd == "OFFSET":
        try:
            if len(words) >= 2 and words[1].upper() == "AUTO":
                js8.offset.enable()
                log("JS8 auto-offset enabled")
            elif len(words) >= 2:
                hz = int(words[1])
                if not OFFSET_MIN <= hz <= OFFSET_MAX:
                    mesh_send(sender, f"Offset must be {OFFSET_MIN}-{OFFSET_MAX} Hz; not changed.")
                    return
                js8.offset.disable()   # otherwise pyjs8call may move it again by itself
                js8.settings.set_offset(hz)
                log("JS8 offset set to", hz, "(auto-offset off)")
            hz = js8.settings.get_offset(update=True)
            auto = (f"auto-move ON ({js8.offset.min_offset}-{js8.offset.max_offset})"
                    if js8.offset.enabled() else "auto-move OFF")
            mesh_send(sender, f"Offset {hz} Hz, {auto}")
        except ValueError:
            mesh_send(sender, "Usage: OF  |  OF <Hz>, e.g. OF 1500  |  OF AUTO")
        except Exception as e:  # noqa: BLE001
            mesh_send(sender, f"OFFSET failed: {e}")

    elif cmd == "HEARD":
        latest = {}
        for s in js8.spots.filter(age=HEARD_SECONDS):
            if s.origin and (s.origin not in latest or s.timestamp > latest[s.origin].timestamp):
                latest[s.origin] = s
        if not latest:
            mesh_send(sender, f"No stations heard in the last {HEARD_SECONDS // 60} min.")
            return
        now = time.time()
        rows = sorted(latest.values(), key=lambda s: s.timestamp, reverse=True)[:HEARD_COUNT]
        parts = []
        for s in rows:
            snr = f" {s.snr:+d}" if isinstance(s.snr, int) else ""
            grid = f" {s.grid}" if s.grid else ""
            parts.append(f"{s.origin}{snr}{grid} {int((now - s.timestamp) // 60)}m")
        mesh_send(sender, f"Heard ({len(latest)}): " + ", ".join(parts))

    elif cmd == "STATUS":
        try:
            hz = js8.settings.get_freq()
            offset = js8.settings.get_offset()
            speed = js8.settings.get_speed()
        except Exception as e:  # noqa: BLE001
            mesh_send(sender, f"STATUS failed: {e}")
            return
        spots = js8.spots.filter(count=1)
        last = f"{int((time.time() - spots[-1].timestamp) // 60)}m ago" if spots else "none yet"
        mesh_send(sender, f"{js8.settings.get_station_callsign()} dial {hz / 1e6:.3f} MHz "
                          f"offset {offset} Hz, speed {speed}, last RX {last}")

    elif cmd == "GROUPS":
        args = [a.upper() for a in text.split()[1:]]
        if args == ["ALL"]:
            new = None
        elif args == ["NONE"]:
            new = set()
        elif args:
            new = {a if a.startswith("@") else "@" + a for a in args}
            bad = [g for g in sorted(new) if not GROUP_RE.match(g) or g == "@HB"]
            if bad:
                mesh_send(sender, f"Not a group name: {' '.join(bad)}. Usage: GR | GR @G1 @G2 | GR ALL | GR NONE")
                return
        else:
            mesh_send(sender, describe_groups(forward_groups))
            return
        forward_groups = new
        try:
            save_groups(new)
        except OSError as e:
            log("Saving groups failed:", e)
            mesh_send(sender, f"{describe_groups(new)} (NOT saved; lost on restart: {e})")
            return
        log("Forward groups set:", describe_groups(new))
        mesh_send(sender, describe_groups(new))

    elif cmd == "CALL":
        try:
            if len(words) >= 2:
                if not hasattr(js8, "set_selected_call"):
                    mesh_send(sender, "Selecting a call needs the Subspace backend; "
                                      "local JS8Call's API can't do it.")
                    return
                want = words[1].upper()
                if want in ("NONE", "-"):
                    want = ""
                elif not CALL_RE.match(want):
                    mesh_send(sender, f"'{want}' doesn't look like a callsign or @group.")
                    return
                got = js8.set_selected_call(want)
                log("JS8 selected call set:", want or "(none)", "->", got)
                if want and (got or "").upper() != want:
                    mesh_send(sender, f"Station didn't select {want}; selected: {got or 'none'}")
                    return
            got = js8.get_selected_call()
            mesh_send(sender, f"Selected call: {got}" if got else "No call selected.")
        except Exception as e:  # noqa: BLE001
            mesh_send(sender, f"CALL failed: {e}")

    elif cmd == "HB":
        try:
            msg = js8.send_heartbeat()
            log("JS8 heartbeat queued")
            mesh_send(sender, f"Heartbeat queued for JS8 TX.{tx_off_warning()}")
            report_tx(sender, msg, "heartbeat")
        except Exception as e:  # noqa: BLE001
            mesh_send(sender, f"HB failed: {e}")

    else:
        mesh_send(sender, "Unknown command. Send HELP.")


# ---- JS8Call side -------------------------------------------------------------
def on_js8_directed(msg):
    dest = str(msg.destination or "").upper()
    mycall = js8.settings.get_station_callsign().upper()
    group_ok = (dest.startswith("@") and dest != "@HB"
                and (forward_groups is None or dest in forward_groups))
    if not (dest == mycall or group_ok):
        return
    # drop JS8's end-of-message mark (often garbled into non-ASCII bytes) and trailing junk
    text = re.sub(r"[^\x20-\x7e]+", " ", msg.text or msg.value or "").strip()
    key = (msg.origin, dest, text)
    now = time.time()
    for k in [k for k, t in recent.items() if now - t > DEDUPE_SECONDS]:
        del recent[k]
    if key in recent:
        return
    recent[key] = now
    snr = f" {msg.snr:+d}dB" if isinstance(msg.snr, int) else ""
    line = f"JS8 {msg.origin}>{dest}{snr}: {text}"
    log("JS8 RX:", line)
    for node in ALLOWED_NODES:
        mesh_send(node, line)


def start_subspace():
    global js8   # set before connecting: events can arrive during start()
    with open(SUBSPACE_PASSCODE_FILE) as f:
        passcode = f.read().strip()
    js8 = client = subspace_remote.SubspaceJS8(SUBSPACE_URL, passcode,
                                               relogin_seconds=SUBSPACE_RELOGIN_SECONDS)
    client.register_directed(on_js8_directed)
    log("Connecting to Subspace at", SUBSPACE_URL, "over WiFi...")
    client.start()
    log("Subspace connected as", client.callsign)
    return client


def start_local():
    global js8   # set before connecting: events can arrive during start()
    js8 = pyjs8call.Client()
    # Never let pyjs8call rewrite ~/.config/JS8Call.ini: writing it while JS8Call runs
    # emptied the file once (2026-11-15). The TCP API settings it wants are already set.
    js8.config.write = lambda: None
    # pyjs8call's exit handler closes JS8Call by default; leave it running instead.
    atexit.unregister(js8.stop)
    atexit.register(js8.stop, terminate_js8call=False)
    js8.callback.register_incoming(on_js8_directed, message_type=pyjs8call.Message.RX_DIRECTED)
    log("Connecting to JS8Call (starting it if needed)...")
    js8.start()
    js8.js8call.enable_logging()   # API traffic to ~/pyjs8call.log, for diagnosing TX problems
    log("JS8Call connected as", js8.settings.get_station_callsign())
    return js8


def main():
    global js8, mesh

    if JS8_BACKEND == "subspace":
        try:
            js8 = start_subspace()
        except Exception as e:  # noqa: BLE001
            log("Subspace connection failed:", e)
            os._exit(1)
    else:
        js8 = start_local()

    pub.subscribe(on_mesh_text, "meshtastic.receive.text")
    pub.subscribe(on_mesh_lost, "meshtastic.connection.lost")
    log("Connecting to Meshtastic node", MESH_NODE, "over Bluetooth...")
    try:
        mesh = BLEInterface(MESH_NODE)
    except Exception as e:  # noqa: BLE001
        log("Bluetooth connection failed:", e)
        js8.stop(terminate_js8call=False)
        os._exit(1)
    log("Meshtastic connected. Allowed nodes:", ", ".join(sorted(ALLOWED_NODES)))
    threading.Thread(target=mesh_sender, daemon=True).start()

    while True:
        time.sleep(60)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("Stopped.")
        os._exit(0)

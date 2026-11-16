#!/usr/bin/env python3
"""JS8Call <-> Meshtastic gateway (barebones).

Runs on this PC, connected to JS8Call (via pyjs8call) and to a Meshtastic node over
Bluetooth (via the meshtastic library). From an allowed Meshtastic node, send a direct
message (DM) to the gateway node with one of these commands:

    SEND <CALL or @GROUP> <message>   transmit a JS8 directed message as your callsign
    INBOX                             list the latest JS8Call inbox messages
    HEARD                             stations heard in the last hour
    STATUS                            dial, offset, speed, last RX
    FREQ [MHz]                        show, or set, the radio's dial frequency
    OFFSET [Hz|AUTO]                  show or set the TX audio offset (setting turns auto-move off)
    HB                                transmit a JS8 heartbeat
    PING                              check the gateway is up
    HELP                              list commands

Received JS8 messages addressed to your callsign or to an @group (except @HB heartbeats)
are DMed to every allowed node.

Run with ~/meshtastic-venv/bin/python, or use run-js8mesh.sh, which restarts the
gateway if the Bluetooth link drops.
"""
import atexit
import os
import queue
import re
import threading
import time

import pyjs8call
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
# ---------------------------------------------------------------------------

# Short forms: first two letters of each command. HE is HEARD; HELP is H or ?.
ALIASES = {"SE": "SEND", "IN": "INBOX", "HE": "HEARD", "ST": "STATUS", "FR": "FREQ",
           "OF": "OFFSET", "PI": "PING", "H": "HELP"}
OFFSET_MIN, OFFSET_MAX = 300, 2700    # audio offset range OFFSET accepts (Hz)

CALL_RE = re.compile(r"^(@[A-Z0-9]{1,8}|[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}(/[A-Z0-9]{1,4})?)$")

mesh_out = queue.Queue()              # (destination node id, text)
recent = {}                           # dedupe: key -> time seen
js8 = None
mesh = None


def log(*args):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *args, flush=True)


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
def report_tx(sender, msg, label):
    """Follow a queued JS8 message and tell the sender whether it was transmitted."""
    def watch():
        last = None
        end = time.time() + TX_REPORT_SECONDS
        while time.time() < end:
            status = getattr(msg, "status", None)
            if status != last:
                log(f"JS8 TX status [{label}]: {status} | pyjs8call queue "
                    f"{len(js8.js8call._tx_queue)}, tx box {js8.js8call.get_state('tx_text')!r}")
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


def handle_command(sender, text):
    words = text.split(None, 2)
    cmd = words[0].upper().lstrip(".") if words else ""   # a leading "." is optional
    cmd = ALIASES.get(cmd, cmd)

    if cmd in ("HELP", "?"):
        mesh_send(sender, "JS8 gateway (short forms in caps, leading . optional): "
                          "SEnd <CALL|@GROUP> <msg> | INbox | HEard | STatus | FReq [MHz] | "
                          "OFfset [Hz|AUTO] | "
                          "HB | PIng | H/?")

    elif cmd == "PING":
        state = "connected" if js8.connected() else "NOT connected"
        mesh_send(sender, f"Gateway up. JS8Call {state} as {js8.settings.get_station_callsign()}.")

    elif cmd == "SEND":
        if len(words) < 3:
            mesh_send(sender, "Usage: SEND <CALL|@GROUP> <message>")
            return
        dest, body = words[1].upper(), words[2].upper()
        if not CALL_RE.match(dest):
            mesh_send(sender, f"'{dest}' doesn't look like a callsign or @group.")
            return
        try:
            msg = js8.send_directed_message(dest, body)
            log("JS8 TX queued:", dest, repr(body))
            mesh_send(sender, f"Queued for JS8 TX: {dest} {body}")
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

    elif cmd == "HB":
        try:
            msg = js8.send_heartbeat()
            log("JS8 heartbeat queued")
            mesh_send(sender, "Heartbeat queued for JS8 TX.")
            report_tx(sender, msg, "heartbeat")
        except Exception as e:  # noqa: BLE001
            mesh_send(sender, f"HB failed: {e}")

    else:
        mesh_send(sender, "Unknown command. Send HELP.")


# ---- JS8Call side -------------------------------------------------------------
def on_js8_directed(msg):
    dest = str(msg.destination or "").upper()
    mycall = js8.settings.get_station_callsign().upper()
    if not (dest == mycall or (dest.startswith("@") and dest != "@HB")):
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


def main():
    global js8, mesh

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

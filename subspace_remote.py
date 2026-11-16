"""JS8 over WiFi: talk to a Subspace Edition station through its phone-remote server.

Subspace's phone remote (File > Phone remote) is a small web server, port 2443 by
default, that speaks the normal JS8Call JSON API (see JS8_Network/RemoteServer.cpp
in avantol/Subspace-Edition):

    POST /login   {"passcode": "..."}  -> {"ok": true, "session": "<token>"}
    POST /api     one API message in, its reply out (X-Session header)
    GET  /events  Server-Sent Events: every broadcast (?s=<token>)

It allows one session at a time: a new login ends the previous one, so logging in
from a phone logs this gateway out, and the other way round.

SubspaceJS8 offers the same small part of pyjs8call's Client that js8mesh.py uses,
so the command code works with either backend.
"""
import json
import re
import threading
import time
from types import SimpleNamespace

import requests

STATUS_QUEUED, STATUS_SENDING, STATUS_SENT, STATUS_FAILED = "queued", "sending", "sent", "failed"


def _log(*args):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *args, flush=True)


def _norm(text):
    """Letters, digits, @ and / only, upper case: for matching sent text loosely."""
    return re.sub(r"[^A-Z0-9@/]", "", (text or "").upper())


class RemoteError(Exception):
    pass


class _Settings:
    def __init__(self, client):
        self._c = client

    def get_station_callsign(self):
        return self._c.callsign

    def get_freq(self, update=False):
        return int(self._c.request("RIG.GET_FREQ")["params"]["DIAL"])

    def set_freq(self, hz):
        self._c.request("RIG.SET_FREQ", params={"DIAL": int(hz)}, expect_reply=False)

    def get_offset(self, update=False):
        return int(self._c.request("RIG.GET_FREQ")["params"]["OFFSET"])

    def set_offset(self, hz):
        self._c.request("RIG.SET_FREQ", params={"OFFSET": int(hz)}, expect_reply=False)

    def get_speed(self):
        name = self._c.request("MODE.GET_SUBMODE_NAME")["params"].get("NAME", "?")
        return str(name).lower()


class _Offset:
    """pyjs8call's auto-offset runs on the client; there is none for the remote."""
    min_offset, max_offset = 0, 0

    def enabled(self):
        return False

    def enable(self):
        raise RemoteError("auto-offset isn't available over WiFi (Subspace remote)")

    def disable(self):
        pass


class _Spots:
    def __init__(self, client):
        self._c = client

    def filter(self, age=None, count=None):
        """Stations heard, oldest first, from Subspace's call activity list."""
        params = self._c.request("RX.GET_CALL_ACTIVITY")["params"]
        now = time.time()
        spots = []
        for call, d in params.items():
            if call.startswith("_") or not isinstance(d, dict) or call == self._c.callsign:
                continue
            ts = (d.get("UTC") or 0) / 1000
            if age is not None and now - ts > age:
                continue
            snr = d.get("SNR")
            spots.append(SimpleNamespace(origin=call, timestamp=ts, grid=(d.get("GRID") or "").strip(),
                                         snr=int(snr) if isinstance(snr, (int, float)) else None))
        spots.sort(key=lambda s: s.timestamp)
        return spots[-count:] if count else spots


class SubspaceJS8:
    def __init__(self, url, passcode, relogin_seconds=120, timeout=15):
        self.url = url.rstrip("/")
        self.passcode = passcode
        self.relogin_seconds = relogin_seconds  # wait this long before taking the session back
        self.timeout = timeout
        self.callsign = ""
        self.grid = ""
        self.settings = _Settings(self)
        self.offset = _Offset()
        self.spots = _Spots(self)
        self._session = None
        self._lock = threading.Lock()
        self._events_up = False
        self._handlers = []          # functions called with each RX.DIRECTED message
        self._pending = []           # outgoing messages waiting for TX.COMPLETE
        self._kicked_at = 0.0        # when another login (the phone) took the session

    # ---- session -----------------------------------------------------------
    def _login(self):
        r = requests.post(f"{self.url}/login", json={"passcode": self.passcode}, timeout=self.timeout)
        if r.status_code == 403:
            raise RemoteError("wrong pass code")
        if r.status_code == 429:
            raise RemoteError("login locked for 60 s after wrong pass codes")
        r.raise_for_status()
        self._session = r.json()["session"]
        _log("Subspace: logged in at", self.url)

    def _ensure_session(self):
        with self._lock:
            if self._session:
                return self._session
            wait = self._kicked_at + self.relogin_seconds - time.time()
            if wait > 0:
                raise RemoteError(f"another device has the Subspace remote; retaking it in {int(wait)} s")
            self._login()
            return self._session

    def _lost_session(self, token):
        with self._lock:
            if self._session == token:
                self._session = None
                self._kicked_at = time.time()
                _log("Subspace: session ended (another login, e.g. the phone?); "
                     f"retaking it in {self.relogin_seconds} s")

    def request(self, type_, value="", params=None, expect_reply=True):
        """Send one API message and return its reply (a dict)."""
        token = self._ensure_session()
        body = {"type": type_, "value": value, "params": params or {}}
        try:
            r = requests.post(f"{self.url}/api", data=json.dumps(body), headers={"X-Session": token},
                              timeout=self.timeout if expect_reply else 3)
        except requests.exceptions.ReadTimeout:
            if expect_reply:
                raise RemoteError(f"{type_}: no reply from Subspace")
            return {}  # a set command with no reply: the server holds the request open
        if r.status_code == 401:
            self._lost_session(token)
            raise RemoteError("logged out of the Subspace remote")
        r.raise_for_status()
        reply = r.json()
        if reply.get("type") == "API.NO_REPLY":
            if expect_reply:
                raise RemoteError(f"{type_}: no reply from Subspace")
            return {}
        if (reply.get("params") or {}).get("REFUSED"):
            raise RemoteError(f"Subspace refused {type_}: {reply['params'].get('REASON', '?')}")
        return reply

    # ---- the parts of pyjs8call's Client that js8mesh uses -----------------
    def start(self):
        self._ensure_session()
        self.callsign = self.request("STATION.GET_CALLSIGN").get("value", "").upper()
        self.grid = self.request("STATION.GET_GRID").get("value", "")
        threading.Thread(target=self._events_loop, daemon=True).start()

    def stop(self, **_):
        pass

    def connected(self):
        return bool(self._session) and self._events_up

    def register_directed(self, handler):
        self._handlers.append(handler)

    def tx_enabled(self):
        return bool(self.request("TX.GET_ENABLED")["params"].get("ENABLED"))

    def _queue(self, to, cmd):
        msg = SimpleNamespace(status=STATUS_QUEUED, composed="", error="")
        self._pending.append(msg)
        try:
            reply = self.request("TX.SEND_DIRECTED", params={"TO": to, "CMD": cmd, "PRIORITY": "NORMAL"})
        except Exception:
            self._pending.remove(msg)
            raise
        p = reply.get("params", {})
        if not p.get("OK"):
            self._pending.remove(msg)
            raise RemoteError(p.get("ERROR") or "not queued")
        msg.composed = p.get("COMPOSED", "")
        return msg

    def send_directed_message(self, dest, text):
        return self._queue(dest, text)

    def send_heartbeat(self):
        return self._queue("@HB", f"HEARTBEAT {self.grid[:4]}".strip())

    def get_inbox_messages(self, unread=False):
        out = []
        for m in self.request("INBOX.GET_MESSAGES")["params"].get("MESSAGES", []):
            p = m.get("params", {})
            try:
                ts = time.mktime(time.strptime(p.get("UTC", ""), "%Y-%m-%d %H:%M:%S")) - time.timezone
            except ValueError:
                ts = 0
            kind = str(m.get("type", "")).lower()
            out.append({"timestamp": ts, "utc_time_str": time.strftime("%H:%M:%S UTC", time.gmtime(ts)),
                        "origin": p.get("FROM", "?"), "destination": p.get("TO", "?"),
                        "text": (p.get("TEXT") or "").strip(), "value": m.get("value", ""),
                        "unread": kind == "unread"})
        return [m for m in out if m["unread"]] if unread else out

    def get_selected_call(self):
        return self.request("RX.GET_CALL_SELECTED").get("value") or None

    def set_selected_call(self, call):
        """Select a callsign ("" deselects); returns the selection that resulted."""
        return self.request("RX.SET_CALL_SELECTED", value=call).get("value") or None

    def tx_debug(self):
        return "queued here: " + ", ".join(f"{m.composed!r} {m.status}" for m in self._pending)

    # ---- event stream --------------------------------------------------------
    def _events_loop(self):
        while True:
            try:
                token = self._ensure_session()
                with requests.get(f"{self.url}/events", params={"s": token}, stream=True,
                                  timeout=(self.timeout, 60)) as r:   # keepalive comes every 15 s
                    if r.status_code == 401:
                        self._lost_session(token)
                        continue
                    r.raise_for_status()
                    self._events_up = True
                    _log("Subspace: event stream open")
                    for line in r.iter_lines(decode_unicode=True):
                        if line and line.startswith("data: "):
                            try:
                                self._on_event(json.loads(line[6:]))
                            except Exception as e:  # noqa: BLE001 - keep the stream alive
                                _log("Subspace: bad event:", e)
                # the server closed the stream: another login ended our session
                self._lost_session(token)
            except RemoteError as e:
                _log("Subspace:", e)
            except Exception as e:  # noqa: BLE001
                _log("Subspace: event stream error:", e)
            self._events_up = False
            time.sleep(10)

    def _on_event(self, ev):
        t, p = ev.get("type"), ev.get("params") or {}
        if t == "RX.DIRECTED":
            frm, to = p.get("FROM", ""), p.get("TO", "")
            text = p.get("TEXT") or ev.get("value") or ""
            # TEXT is "FROM: TO body"; keep the body
            text = re.sub(rf"^{re.escape(frm)}:\s*", "", text.strip())
            text = re.sub(rf"^{re.escape(to)}>*\s*", "", text)
            snr = p.get("SNR")
            msg = SimpleNamespace(origin=frm, destination=to.rstrip(">"), text=text, value=text,
                                  snr=int(snr) if isinstance(snr, (int, float)) else None)
            for h in self._handlers:
                h(msg)
        elif t == "TX.FRAME":
            for m in self._pending:
                if m.status == STATUS_QUEUED:
                    m.status = STATUS_SENDING
                break
        elif t == "TX.COMPLETE":
            sent = _norm(ev.get("value"))
            match = next((m for m in self._pending if sent and m.composed and
                          (_norm(m.composed) in sent or sent in _norm(m.composed))), None)
            if match is None:
                _log("Subspace: TX.COMPLETE for something not sent by the gateway:", repr(ev.get("value")))
                return
            match.status = STATUS_SENT
            self._pending.remove(match)

# JS8Call ↔ Meshtastic gateway commands

Send these as a **direct message to Rpt1** from an allowed node (currently Wio1, `!8a31cb4a`).
Commands are not case-sensitive, a leading `.` is optional, and the short form in the first column works the same as the full name.

| Short | Full command | What it does | Transmits on HF? |
|---|---|---|---|
| `se` | `SEND <CALL> <msg>` | Send a JS8 message as WM8Q, e.g. `se wd4kav snr?`. Replies "Queued", then "Transmitted" or "did NOT transmit". | Yes |
| `se` | `SEND @<GROUP> <msg>` | Same, to a group, e.g. `se @allcall cq` | Yes |
| `se` | `SEND <msg>` | No callsign given: send to the call or @group selected at the station (see `CALL`), e.g. `se snr?`. A first word that looks like a callsign (letters and a digit, like `kd8skz`) is taken as the destination; `73` is not. | Yes |
| `cq` | `CQ` | Subspace CQ button: Clears any selected call first. (Subspace only) | Yes |
| `re` | `REPLY [CALL]` | Subspace REPLY button: Send standard reply to CALL (selecting it), or for the currently-selected call, e.g. `re kd8skz`. Error if neither, or if a group is selected. (Subspace only) | Yes |
| `sn` | `SNR [CALL]` | Subspace SNR button: Send signal report for CALL (or the currently-selected call) its signal report, e.g. `sn kd8skz`. Error if neither, or if a group is selected. (Subspace only) | Yes |
| `hb` | `HB` | Send a heartbeat (`@HB HEARTBEAT DN61`), then report whether it went out | Yes |
| `in` | `INBOX` | List latest 5 Inbox messages (`*` = unread) | No |
| `he` | `HEARD` | List up to 8 stations heard in the last hour: callsign, SNR, grid, minutes ago | No |
| `st` | `STATUS` | Dial frequency, offset, JS8 speed, last received | No |
| `fr` | `FREQ` | Show the dial frequency and band | No |
| `fr` | `FREQ <MHz>` | Move the radio's VFO dial, e.g. `fr 7.078`. Refuses frequencies outside the ham bands. | No |
| `of` | `OFFSET` | Show the TX audio offset and whether auto-move is on | No |
| `of` | `OFFSET <Hz>` | Set the TX audio offset (300–2700 Hz), e.g. `of 1500`. Turns auto-move off so it stays put. | No |
| `of` | `OFFSET AUTO` | Turn auto-move back on (local JS8Call only; not available over WiFi to Subspace) | No |
| `gr` | `GROUPS` | Show which @groups are forwarded to you | No |
| `gr` | `GROUPS @<G> [@<G> ...]` | Forward only these groups, e.g. `gr @magnet @allcall` (the `@` is optional) | No |
| `gr` | `GROUPS ALL` / `GROUPS NONE` | Forward every group / no groups (the default), only messages to WM8Q | No |
| `ca` | `CALL` | Show the callsign (or @group) currently selected at the station | No |
| `ca` | `CALL <CALL>` / `CALL NONE` | Select a callsign or @group, e.g. `ca kd8skz`, or clear the selection. Subspace backend only. | No |
| `pi` | `PING` | Check the gateway is up and connected to JS8Call or Subspace | No |
| `h` / `?` | `HELP` | List the commands | No |

## Notes

- **Automatic forwarding:** JS8 messages addressed to WM8Q, or to an @group chosen with `GROUPS`, are DMed to you without asking. Heartbeats (`@HB`) are always skipped, and repeats within 10 minutes are dropped.
- **Auto-move:** when on, the offset shifts by itself (within 1000–2500 Hz) if another station is on your frequency. It comes back on whenever the gateway restarts.
- **CQ, REPLY, SNR** press the station's own buttons, so they send what those buttons are set to send there. They need the Subspace backend.
- **Over WiFi (Subspace backend):** the same commands work. Auto-move is always off. `SEND` and `HB` add a warning if TX is switched off at the station. Logging in from the Subspace phone remote logs the gateway out, and the gateway logs back in (logging the phone out) 2 minutes later.
- **Group choice is saved** in `~/.config/js8mesh/forward-groups.json`, so it survives restarts.
- **Other nodes:** commands from nodes not on the allowed list are ignored.
- **Long replies** are split into numbered parts, e.g. `(1/2)`, sent a few seconds apart.

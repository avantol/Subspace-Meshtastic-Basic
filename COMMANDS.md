# JS8Call ↔ Meshtastic gateway commands

Send these as a **direct message to Rpt1** from an allowed node (currently Wio1, `!4b7e12ad`).
Commands are not case-sensitive, a leading `.` is optional, and the short form in the first column works the same as the full name.

| Short | Full command | What it does | Transmits on HF? |
|---|---|---|---|
| `se` | `SEND <CALL> <msg>` | Send a JS8 message as WM8Q, e.g. `se wd4kav snr?`. Replies "Queued", then "Transmitted" or "did NOT transmit". | Yes |
| `se` | `SEND @<GROUP> <msg>` | Same, to a group, e.g. `se @allcall cq` | Yes |
| `hb` | `HB` | Send a JS8 heartbeat (`@HB HEARTBEAT DN61`), then report whether it went out | Yes |
| `in` | `INBOX` | Latest 5 JS8Call inbox messages (`*` = unread) | No |
| `he` | `HEARD` | Up to 8 stations heard in the last hour: callsign, SNR, grid, minutes ago | No |
| `st` | `STATUS` | Dial frequency, offset, JS8 speed, last received | No |
| `fr` | `FREQ` | Show the dial frequency and band | No |
| `fr` | `FREQ <MHz>` | Move the IC-7300's dial, e.g. `fr 7.078`. Refuses frequencies outside the ham bands. | No |
| `of` | `OFFSET` | Show the TX audio offset and whether auto-move is on | No |
| `of` | `OFFSET <Hz>` | Set the offset (300–2700 Hz), e.g. `of 1500`. Turns auto-move off so it stays put. | No |
| `of` | `OFFSET AUTO` | Turn auto-move back on | No |
| `pi` | `PING` | Check the gateway is up and connected to JS8Call | No |
| `h` / `?` | `HELP` | List the commands | No |

## Notes

- **Automatic forwarding:** JS8 messages addressed to WM8Q or to an @group are DMed to you without asking. Heartbeats (`@HB`) are skipped, and repeats within 10 minutes are dropped.
- **Auto-move:** when on, the offset shifts by itself (within 1000–2500 Hz) if another station is on your frequency. It comes back on whenever the gateway restarts.
- **Other nodes:** commands from nodes not on the allowed list are ignored.
- **Long replies** are split into numbered parts, e.g. `(1/2)`, sent a few seconds apart.

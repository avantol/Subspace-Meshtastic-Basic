#!/bin/bash
# Run the JS8Call <-> Meshtastic gateway and restart it if it exits (e.g. Bluetooth drops).
# Stop with Ctrl+C.
cd "$(dirname "$0")"
trap 'exit 0' INT TERM
while true; do
    # Drop any stale BLE link left by a previous run: a connected node stops advertising,
    # so the gateway couldn't find it by name.
    bluetoothctl disconnect FD:08:0B:7B:BE:91 >/dev/null 2>&1
    ~/meshtastic-venv/bin/python js8mesh.py 2>&1 | tee -a js8mesh.log
    echo "$(date '+%F %T') gateway exited; restarting in 15 s" | tee -a js8mesh.log
    sleep 15
done

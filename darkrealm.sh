#!/bin/bash
# Start-Script fuer ArkOS: liegt in /roms/ports/, das Spiel in /roms/ports/darkrealm/
cd "$(dirname "$0")/darkrealm" || exit 1
chmod +x ./darkrealm
./darkrealm > log.txt 2>&1

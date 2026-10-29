#!/bin/sh
# Start VarAC in the 32-bit Wine prefix made for testing. VarAC launches
# VARA HF itself (VarahfLaunchOnModemConnect=ON); starting VARA by hand as
# well gives "two copies of VARA". If anything is stuck first run:
#   WINEPREFIX=$HOME/.wine-varac wineserver -k
#   sh tools/start-varac-wine.sh
export WINEPREFIX="$HOME/.wine-varac"
export DISPLAY="${DISPLAY:-:0.0}"
cd "$WINEPREFIX/drive_c/VarAC" || exit 1
nohup wine VarAC.exe > /tmp/varac.log 2>&1 &
echo "VarAC started (log: /tmp/varac.log); it launches VARA HF itself"

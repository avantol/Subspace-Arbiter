#!/bin/sh
# The Arbiter with its window: the C++ build if it exists, else the Python
# reference version. Close the window to stop it; every hold is released on
# the way out. Never run two arbiters at once.
cd /home/john/JS8Call/arbiter || exit 1
if [ -x build/arbiter ]; then
  exec build/arbiter "$@"
fi
exec python3 arbiter.py "$@"

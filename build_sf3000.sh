#!/bin/sh
set -e
cd /home/tomaszz/sf3000-work/pcsx4all
make -f Makefile.sf3000 clean 2>&1 | tail -3
make -f Makefile.sf3000 -j4 2>&1 | tail -40
echo "=== build complete ==="
ls -la pcsx4all
file pcsx4all 2>/dev/null || true

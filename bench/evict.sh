#!/bin/sh
# Empties the OS file cache so the next run reads from disk.
#
#   bench/evict.sh [DIR_TO_STREAM]
#
# Uses `purge` when it can run as root (run `sudo -v` first, or `sudo bench/evict.sh`).
# Otherwise it streams ~30 GB of unrelated *.bin files through the page cache, which on a
# 16 GB machine pushes everything else out. Pass a directory whose files the next run does
# NOT use (default: out/, the raw files; for a raw->store ingest use the old store dir).
set -e
if [ "$(id -u)" = 0 ]; then purge; echo "evict: purge"; exit 0; fi
if sudo -n true 2>/dev/null; then sudo purge; echo "evict: sudo purge"; exit 0; fi
DIR=${1:-out}
find "$DIR" -type f \( -name '*.bin' -o -name '*.db' \) | head -n 90000 | xargs cat > /dev/null
echo "evict: streamed files from $DIR (no root for purge)"

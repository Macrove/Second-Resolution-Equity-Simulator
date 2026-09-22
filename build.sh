#!/bin/sh
# Builds everything and puts ./gen ./ingest ./sim next to this script, so the commands
# in the assignment (./gen ..., ./ingest ..., ./sim ...) work as written.
#   ./build.sh          build (Release)
#   ./build.sh test     build and run the tests
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j
cp build/gen build/ingest build/sim .
if [ "$1" = "test" ]; then (cd build && ctest --output-on-failure); fi

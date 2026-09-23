#!/bin/sh
# Benchmarks on an existing full-quarter store. Cold runs call bench/evict.sh first
# (purge if sudo works, else streams raw files through the page cache).
#   caffeinate -i bench/run_all.sh
cd "$(dirname "$0")/.."
S=${STORE:-store}
ev() { bench/evict.sh "${EVICT_DIR:-out}" | tail -1; }
echo "=== snapshot, cold";  ev; ./sim snapshot --store $S --in bench/data/timestamps.txt --out /tmp/snap.bin 2>&1 | grep -E "latency|wall"
echo "=== trades (blocks), cold"; ev; ./sim trades --store $S --in bench/data/trades.csv --out /tmp/res.csv 2>&1 | grep -E "priced|wall"
echo "=== trades (blocks), warm"; ./sim trades --store $S --in bench/data/trades.csv --out /tmp/res.csv 2>&1 | grep -E "priced|wall"
echo "=== trades (scan baseline), cold"; ev; ./sim trades --store $S --in bench/data/trades.csv --out /tmp/res_scan.csv --mae scan 2>&1 | grep -E "priced|wall"
cmp /tmp/res.csv /tmp/res_scan.csv && echo "blocks output == scan output"
echo "=== strategy W=1800 entry=2 exit=0 hold=3600, cold"; ev
./sim strategy --store $S --reverters out/reverters.txt --window 1800 --entry 2.0 --exit 0.0 --max-hold 3600 --out /tmp/st1.csv --summary /tmp/sum1.csv 2>&1 | grep -vE "^loaded"
echo "=== strategy W=600 entry=1.5 exit=0.5 hold=1800, warm"
./sim strategy --store $S --reverters out/reverters.txt --window 600 --entry 1.5 --exit 0.5 --max-hold 1800 --out /tmp/st2.csv --summary /tmp/sum2.csv 2>&1 | grep -vE "^loaded"
echo "=== strategy W=300 entry=3 exit=0 hold=7200, warm"
./sim strategy --store $S --reverters out/reverters.txt --window 300 --entry 3.0 --exit 0.0 --max-hold 7200 --out /tmp/st3.csv --summary /tmp/sum3.csv 2>&1 | grep -vE "^loaded"
echo "=== done"

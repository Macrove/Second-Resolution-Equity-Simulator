#!/usr/bin/env python3
"""Random benchmark inputs: 1M positions (trades.csv) and 10k snapshot times.

  python3 bench/make_inputs.py --out bench/data [--trades 1000000] [--snapshots 10000] [--seed 1]

Snapshot times are uniform over the quarter 2025-01-02 .. 2025-03-31 UTC, nights and
weekends included. Position entries: 85% land inside the instrument's (approximate, DST-
widened) trading hours on a weekday, 15% anywhere in the quarter. Holding times are
log-uniform from 1 second to 5 days.
"""
import argparse
import os

import numpy as np

NS = 1_000_000_000
START = 1735776000 * NS  # 2025-01-02T00:00:00Z
END = 1743465600 * NS    # 2025-04-01T00:00:00Z


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="bench/data")
    ap.add_argument("--trades", type=int, default=1_000_000)
    ap.add_argument("--snapshots", type=int, default=10_000)
    ap.add_argument("--instruments", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    os.makedirs(a.out, exist_ok=True)

    n = a.trades
    inst = rng.integers(0, a.instruments, n)
    side = rng.choice([-1, 1], n)
    qty = rng.integers(1, 1001, n)
    # Approximate UTC trading windows per region (widened to cover DST), in seconds of day.
    lo = np.where(inst < 1000, 13 * 3600 + 1800, np.where(inst < 1500, 7 * 3600, 0))
    hi = np.where(inst < 1000, 21 * 3600, np.where(inst < 1500, 16 * 3600 + 1800, 6 * 3600))
    day = rng.integers(0, 89, n)
    weekday = (day + 3) % 7 < 5  # 2025-01-02 is a Thursday: day 0 -> 3
    day = np.where(weekday, day, day - 1 - ((day + 3) % 7 - 5))  # weekend -> previous Friday
    secs = lo + (rng.random(n) * (hi - lo)).astype(np.int64)
    in_session = START + (day * 86400 + secs) * NS
    anywhere = rng.integers(START, END, n)
    entry = np.where(rng.random(n) < 0.85, in_session, anywhere)
    hold = np.exp(rng.uniform(0, np.log(5 * 86400), n)) * NS
    exit_ = entry + hold.astype(np.int64)
    with open(os.path.join(a.out, "trades.csv"), "w") as f:
        f.write("trade_id,instrument_id,side,quantity,entry_ts_ns,exit_ts_ns\n")
        f.write("\n".join(f"{i},{inst[i]},{side[i]},{qty[i]},{entry[i]},{exit_[i]}" for i in range(n)))
        f.write("\n")

    ts = rng.integers(START, END, a.snapshots)
    with open(os.path.join(a.out, "timestamps.txt"), "w") as f:
        f.write("\n".join(str(t) for t in ts) + "\n")


if __name__ == "__main__":
    main()

// Tests for sim: positions, snapshots and the strategy. Small hand-computed
// datasets pin down exact answers (snapping, gaps, weekend, Tokyo lunch, MAE,
// segment resets); randomised checks compare against straightforward
// reimplementations of the spec on generated data.
#include <cmath>
#include <random>

#include "sim_cmds.h"
#include "test_util.h"

using namespace sim;

static int64_t ns(int64_t day, int64_t sec) { return (day * 86400 + sec) * 1'000'000'000LL; }

constexpr int64_t kFri = 20091;  // 2025-01-03
constexpr int64_t kMon = 20094;  // 2025-01-06
constexpr int64_t kTue = 20095;  // 2025-01-07
constexpr int64_t kUsOpen = 14 * 3600 + 1800;  // 09:30 EST in UTC seconds

// One record per second from (day, startSec).
static std::vector<Record> series(int64_t day, int64_t startSec, const std::vector<float>& prices) {
    std::vector<Record> r;
    for (size_t i = 0; i < prices.size(); ++i) r.push_back({ns(day, startSec + static_cast<int64_t>(i)), prices[i], 1});
    return r;
}

static void ingest(const fs::path& raw, const fs::path& store, int stride) {
    const std::string cmd = std::string(INGEST_BIN) + " --raw " + raw.string() + " --store " + store.string() +
                            " --config " + CONFIG_PATH + " --stride " + std::to_string(stride);
    CHECK(runCmd(cmd) == 0, cmd);
}

static std::string simCmd(const std::string& args) {
    return std::string(SIM_BIN) + " " + args + " --runlog none 2>/dev/null";
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------------------
// Trades

struct TradeCase {
    const char* name;
    TradeIn in;
    bool ok;
    int64_t entryNs, exitNs;
    float entryPrice, exitPrice;
    double pnl, mae;
};

static void testTradesHand(const fs::path& tmp) {
    const fs::path raw = tmp / "trades_raw";
    writeRecords(fileFor(raw, "20250103", 0), series(kFri, kUsOpen, {100, 101, 99, 97, 102, 103, 104, 100, 100, 100}));
    writeRecords(fileFor(raw, "20250106", 0), series(kMon, kUsOpen, {110, 111, 112, 109, 108, 110, 115, 111, 110, 110}));
    // Tokyo 2025-01-06: morning 09:00 JST = 00:00 UTC, afternoon 12:30 JST = 03:30 UTC.
    auto tokyo = series(kMon, 0, {50, 51, 52, 53, 54});
    const auto afternoon = series(kMon, 3 * 3600 + 1800, {40, 41, 42, 43, 44});
    tokyo.insert(tokyo.end(), afternoon.begin(), afternoon.end());
    writeRecords(fileFor(raw, "20250106", 1500), tokyo);
    const fs::path store = tmp / "trades_store";
    ingest(raw, store, 3);  // small stride so blocks are used by the MAE lookup

    const int64_t fri = kFri, mon = kMon;
    auto T = [](int64_t inst, int side, int64_t qty, int64_t entry, int64_t exit) {
        TradeIn t;
        t.instrument = inst; t.side = side; t.quantity = qty; t.entryNs = entry; t.exitNs = exit;
        return t;
    };
    const TradeCase cases[] = {
        {"entry before data snaps forward", T(0, 1, 10, ns(fri, kUsOpen - 100), ns(fri, kUsOpen + 5)),
         true, ns(fri, kUsOpen), ns(fri, kUsOpen + 5), 100, 103, 30, 30},                  // low 97 -> 3 * 10
        {"short, loss from the high", T(0, -1, 10, ns(fri, kUsOpen + 1), ns(fri, kUsOpen + 6)),
         true, ns(fri, kUsOpen + 1), ns(fri, kUsOpen + 6), 101, 104, -30, 30},              // high 104 -> 3 * 10
        {"entry in overnight gap, exit after data ends", T(0, 1, 5, ns(fri, 72000), ns(mon, 82800)),
         true, ns(mon, kUsOpen), ns(mon, kUsOpen + 9), 110, 110, 0, 10},                    // low 108 -> 2 * 5
        {"spans the weekend", T(0, 1, 1, ns(fri, kUsOpen + 8), ns(mon, kUsOpen + 3)),
         true, ns(fri, kUsOpen + 8), ns(mon, kUsOpen + 3), 100, 109, 9, 0},
        {"exit before entry after snapping", T(0, 1, 1, ns(fri, 72000), ns(fri, 75600)), false, 0, 0, 0, 0, 0, 0},
        {"entry after last record", T(0, 1, 1, ns(kTue, kUsOpen), ns(kTue, kUsOpen + 100)), false, 0, 0, 0, 0, 0, 0},
        {"exit before first record", T(0, 1, 1, ns(20089, 0), ns(20089, 600)), false, 0, 0, 0, 0, 0, 0},
        {"instrument outside the store", T(1999, 1, 1, ns(fri, 0), ns(mon, 0)), false, 0, 0, 0, 0, 0, 0},
        {"instrument without data", T(1400, 1, 1, ns(fri, 0), ns(mon, 0)), false, 0, 0, 0, 0, 0, 0},
        {"entry equals exit", T(0, 1, 3, ns(fri, kUsOpen + 4), ns(fri, kUsOpen + 4)),
         true, ns(fri, kUsOpen + 4), ns(fri, kUsOpen + 4), 102, 102, 0, 0},
        {"both inside the Tokyo lunch", T(1500, 1, 2, ns(kMon, 7200), ns(kMon, 9900)), false, 0, 0, 0, 0, 0, 0},
        {"long across the Tokyo lunch", T(1500, 1, 2, ns(kMon, 0), ns(kMon, 3 * 3600 + 1800 + 4)),
         true, ns(kMon, 0), ns(kMon, 3 * 3600 + 1800 + 4), 50, 44, -12, 20},                // low 40 -> 10 * 2
        {"short across the Tokyo lunch", T(1500, -1, 1, ns(kMon, 2), ns(kMon, 3 * 3600 + 1800 + 1)),
         true, ns(kMon, 2), ns(kMon, 3 * 3600 + 1800 + 1), 52, 41, 11, 2},                  // high 54 -> 2 * 1
    };

    Store s(store.string());
    for (MaeMode mode : {MaeMode::Blocks, MaeMode::Scan}) {
        for (const TradeCase& c : cases) {
            const TradeOut r = priceTrade(s, c.in, mode);
            CHECK(r.ok == c.ok, c.name);
            if (!c.ok || !r.ok) continue;
            CHECK(r.entryNs == c.entryNs && r.exitNs == c.exitNs, c.name << " snapped timestamps");
            CHECK(r.entryPrice == c.entryPrice && r.exitPrice == c.exitPrice, c.name << " prices");
            CHECK(std::abs(r.pnl - c.pnl) < 1e-9, c.name << " pnl " << r.pnl);
            CHECK(std::abs(r.mae - c.mae) < 1e-9, c.name << " mae " << r.mae);
        }
    }

    // Through the CLI: exact CSV text, header, status 1 rows with empty numbers.
    std::string csv = "trade_id,instrument_id,side,quantity,entry_ts_ns,exit_ts_ns\n";
    int n = 0;
    for (const TradeCase& c : cases)
        csv += "t" + std::to_string(n++) + "," + std::to_string(c.in.instrument) + "," + std::to_string(c.in.side) + "," +
               std::to_string(c.in.quantity) + "," + std::to_string(c.in.entryNs) + "," +
               std::to_string(c.in.exitNs) + "\n";
    writeBytes(tmp / "trades_in.csv", csv.data(), csv.size());
    for (const char* mae : {"blocks", "scan"}) {
        const fs::path out = tmp / (std::string("trades_out_") + mae + ".csv");
        CHECK(runCmd(simCmd("trades --store " + store.string() + " --in " + (tmp / "trades_in.csv").string() +
                         " --out " + out.string() + " --threads 3 --mae " + mae)) == 0, "sim trades");
        const std::string text = slurp(out);
        CHECK(text.rfind("trade_id,status,entry_ts_ns,exit_ts_ns,entry_price,exit_price,pnl,max_adverse_excursion\n", 0) == 0, "header");
        CHECK(text.find("t4,1,,,,,,\n") != std::string::npos, "unresolved row format");
        const std::string want0 = "t0,0," + std::to_string(cases[0].entryNs) + "," + std::to_string(cases[0].exitNs) + ",100,103,30,30\n";
        CHECK(text.find(want0) != std::string::npos, "first row: " << text.substr(0, 300));
        CHECK(std::count(text.begin(), text.end(), '\n') == 1 + static_cast<long>(sizeof cases / sizeof cases[0]), "row count");
    }
    CHECK(slurp(tmp / "trades_out_blocks.csv") == slurp(tmp / "trades_out_scan.csv"), "blocks and scan agree");
}

// ---------------------------------------------------------------------------
// Snapshots

static void testSnapshotHand(const fs::path& tmp) {
    const fs::path store = tmp / "trades_store";  // built by testTradesHand
    Store s(store.string());
    struct Case { int64_t ts; float us, tokyo; };
    const float nan = std::nanf("");
    const Case cases[] = {
        {ns(20089, 0), nan, nan},                          // before everything
        {ns(kFri, kUsOpen + 3), 97, nan},                  // mid-day; Tokyo has no data yet
        {ns(kFri, 72000), 100, nan},                       // after the Friday close
        {ns(kMon, 10800), 100, 54},                        // Tokyo lunch; US not yet open
        {ns(kMon, 3 * 3600 + 1800 + 2), 100, 42},          // Tokyo afternoon
        {ns(kMon, kUsOpen), 110, 44},                      // exactly the first US record
        {ns(kTue, 5), 110, 44},                            // after everything
    };
    ThreadPool pool(4);
    std::vector<float> row(kSnapshotWidth);
    for (const Case& c : cases) {
        snapshotRow(s, c.ts, row.data(), pool);
        auto same = [](float a, float b) { return std::isnan(a) ? std::isnan(b) : a == b; };
        CHECK(same(row[0], c.us), "US at " << c.ts << " got " << row[0]);
        CHECK(same(row[1500], c.tokyo), "Tokyo at " << c.ts << " got " << row[1500]);
        CHECK(std::isnan(row[1]) && std::isnan(row[1999]) && std::isnan(row[1400]), "instruments without data are NaN");
    }

    // CLI: raw float32[N][2000], no header.
    std::string in;
    for (const Case& c : cases) in += std::to_string(c.ts) + "\n";
    writeBytes(tmp / "snap_in.txt", in.data(), in.size());
    CHECK(runCmd(simCmd("snapshot --store " + store.string() + " --in " + (tmp / "snap_in.txt").string() + " --out " +
                     (tmp / "snap_out.bin").string() + " --threads 4")) == 0, "sim snapshot");
    const std::string bin = slurp(tmp / "snap_out.bin");
    CHECK(bin.size() == sizeof cases / sizeof cases[0] * kSnapshotWidth * sizeof(float), "output size " << bin.size());
    if (bin.size() >= 2 * kSnapshotWidth * sizeof(float)) {
        const float* f = reinterpret_cast<const float*>(bin.data());
        CHECK(std::isnan(f[0]) && f[kSnapshotWidth + 0] == 97, "row 0 and row 1 of the file");
    }
}

// ---------------------------------------------------------------------------
// Strategy

static void checkTrade(const StrategyTrade& t, int side, int64_t qty, int64_t entryNs, int64_t exitNs, float entryPrice,
                       float exitPrice, double pnl, const char* what) {
    CHECK(t.side == side && t.quantity == qty, what << " side/qty");
    CHECK(t.entryNs == entryNs && t.exitNs == exitNs, what << " times " << t.entryNs << " " << t.exitNs);
    CHECK(t.entryPrice == entryPrice && t.exitPrice == exitPrice, what << " prices");
    CHECK(std::abs(t.pnl - pnl) < 1e-9, what << " pnl " << t.pnl);
}

// Hand-computed on prices 10 10 10 8 9 10 12 12 11 10 with window 3 (see comments).
static void testStrategyHand() {
    const auto r = series(kFri, kUsOpen, {10, 10, 10, 8, 9, 10, 12, 12, 11, 10});
    const int64_t t0 = ns(kFri, kUsOpen);
    const int64_t s = 1'000'000'000LL;
    std::vector<StrategyTrade> out;

    // i=2 window [10,10,10]: std 0, no signal.
    // i=3 [10,10,8]: z=-1.414 < -1 -> long at 8, qty 1250.
    // i=4 [10,8,9]: z=0 >= 0 -> exit at 9, +1250; no re-entry that second.
    // i=5 [8,9,10]: z=+1.225 > 1 -> short at 10, qty 1000.
    // i=6 z=1.336, i=7 z=0.707: still open. i=8 [12,12,11]: z=-1.414 <= -0 -> exit at 11, -1000.
    //   (z < -1 would open a long, but not in the exit second.)
    // i=9 is the last record: no position is opened.
    strategyOnSegment(r.data(), r.size(), {3, 1.0, 0.0, 100}, out);
    CHECK(out.size() == 2, "trade count " << out.size());
    if (out.size() == 2) {
        checkTrade(out[0], 1, 1250, t0 + 3 * s, t0 + 4 * s, 8, 9, 1250, "long");
        checkTrade(out[1], -1, 1000, t0 + 5 * s, t0 + 8 * s, 10, 11, -1000, "short");
    }

    // Max hold 2s: the short (opened at i=5) leaves at i=7 at 12; a long then opens at i=8 (11, qty 909)
    // and is closed by the segment end at i=9 (10).
    out.clear();
    strategyOnSegment(r.data(), r.size(), {3, 1.0, 0.0, 2}, out);
    CHECK(out.size() == 3, "max-hold trade count " << out.size());
    if (out.size() == 3) {
        checkTrade(out[0], 1, 1250, t0 + 3 * s, t0 + 4 * s, 8, 9, 1250, "hold: long");
        checkTrade(out[1], -1, 1000, t0 + 5 * s, t0 + 7 * s, 10, 12, -2000, "hold: short");
        checkTrade(out[2], 1, 909, t0 + 8 * s, t0 + 9 * s, 11, 10, -909, "hold: long to segment end");
    }

    // Entry threshold above every z: no trades. Window longer than the segment: no trades.
    out.clear();
    strategyOnSegment(r.data(), r.size(), {3, 5.0, 0.0, 100}, out);
    strategyOnSegment(r.data(), r.size(), {11, 0.1, 0.0, 100}, out);
    CHECK(out.empty(), "no trades expected");
}

static void testStrategyCli(const fs::path& tmp) {
    // Friday and Monday are separate segments: the window must not carry over, an open
    // position closes at the segment's last record, and constant windows give no signal.
    const fs::path raw = tmp / "strat_raw";
    writeRecords(fileFor(raw, "20250103", 0), series(kFri, kUsOpen, {10, 10, 10, 8, 9, 10, 12, 12, 11, 10}));
    writeRecords(fileFor(raw, "20250106", 0), series(kMon, kUsOpen, {10, 10, 10, 8, 8, 8, 8, 8, 8, 8}));
    const fs::path store = tmp / "strat_store";
    ingest(raw, store, 3);
    writeBytes(tmp / "rev.txt", "0\n", 2);

    auto run = [&](const std::string& window, const std::string& hold) {
        return runCmd(simCmd("strategy --store " + store.string() + " --reverters " + (tmp / "rev.txt").string() +
                          " --window " + window + " --entry 1.0 --exit 0.0 --max-hold " + hold + " --threads 2 --out " +
                          (tmp / "st_trades.csv").string() + " --summary " + (tmp / "st_summary.csv").string()));
    };
    const int64_t s = 1'000'000'000LL;
    auto line = [&](int side, int qty, int64_t day, int a, int b, int pa, int pb, int pnl) {
        return "0," + std::to_string(side) + "," + std::to_string(qty) + "," + std::to_string(ns(day, kUsOpen) + a * s) + "," +
               std::to_string(ns(day, kUsOpen) + b * s) + "," + std::to_string(pa) + "," + std::to_string(pb) + "," +
               std::to_string(pnl) + "\n";
    };
    const std::string head = "instrument_id,side,quantity,entry_ts_ns,exit_ts_ns,entry_price,exit_price,pnl\n";

    CHECK(run("3", "100") == 0, "sim strategy");
    CHECK(slurp(tmp / "st_trades.csv") == head + line(1, 1250, kFri, 3, 4, 8, 9, 1250) + line(-1, 1000, kFri, 5, 8, 10, 11, -1000) +
                                              line(1, 1250, kMon, 3, 9, 8, 8, 0),
          "trades csv:\n" << slurp(tmp / "st_trades.csv"));

    // Summary: 3 regions x 2 flags. US reverter: 3 trades, pnl 250 (Fri 250, Mon 0), sharpe = sqrt(126).
    const std::string summary = slurp(tmp / "st_summary.csv");
    CHECK(std::count(summary.begin(), summary.end(), '\n') == 7, "summary rows:\n" << summary);
    CHECK(summary.rfind("region,reverter_flag,trade_count,total_pnl,sharpe\n", 0) == 0, "summary header");
    CHECK(summary.find("US,0,0,0,nan\n") != std::string::npos, "empty group is explicit: " << summary);
    const size_t at = summary.find("US,1,3,250,");
    CHECK(at != std::string::npos, "US reverter row:\n" << summary);
    if (at != std::string::npos) CHECK(std::abs(std::stod(summary.substr(at + 11)) - std::sqrt(126.0)) < 1e-9, "sharpe");

    // A different parameter set changes the output (parameters are not hard-coded).
    CHECK(run("3", "2") == 0, "sim strategy hold 2");
    const std::string t2 = slurp(tmp / "st_trades.csv");
    CHECK(t2 == head + line(1, 1250, kFri, 3, 4, 8, 9, 1250) + line(-1, 1000, kFri, 5, 7, 10, 12, -2000) +
                    line(1, 909, kFri, 8, 9, 11, 10, -909) + line(1, 1250, kMon, 3, 5, 8, 8, 0),
          "hold-2 trades csv:\n" << t2);
    CHECK(run("0", "2") != 0, "window 0 must be rejected");
    CHECK(runCmd(simCmd("strategy --store " + store.string())) != 0, "missing options must fail");
}

// Straight-from-the-spec implementation: recompute the window from scratch every second.
static std::vector<StrategyTrade> naiveStrategy(const Record* r, size_t n, const StrategyParams& p) {
    std::vector<StrategyTrade> out;
    const size_t W = static_cast<size_t>(p.window);
    bool open = false;
    int side = 0;
    int64_t qty = 0, entryNs = 0;
    float entryPrice = 0;
    for (size_t i = 0; i < n; ++i) {
        bool signal = false;
        double z = 0;
        if (i + 1 >= W) {
            double mean = 0;
            for (size_t k = i + 1 - W; k <= i; ++k) mean += r[k].price;
            mean /= static_cast<double>(W);
            double var = 0;
            for (size_t k = i + 1 - W; k <= i; ++k) var += (r[k].price - mean) * (r[k].price - mean);
            var /= static_cast<double>(W);
            if (var > 0) {
                z = (r[i].price - mean) / std::sqrt(var);
                signal = true;
            }
        }
        const bool last = i + 1 == n;
        if (open) {
            const bool hit = signal && (side > 0 ? z >= p.exit : z <= -p.exit);
            if (hit || r[i].timestampNs - entryNs >= p.maxHoldSec * 1'000'000'000LL || last) {
                out.push_back({entryNs, r[i].timestampNs, entryPrice, r[i].price, qty, side,
                               side * static_cast<double>(qty) * (static_cast<double>(r[i].price) - entryPrice), 0});
                open = false;
            }
        } else if (signal && !last && (z > p.entry || z < -p.entry)) {
            side = z > p.entry ? -1 : 1;
            qty = static_cast<int64_t>(std::floor(10000.0 / r[i].price));
            open = true;
            entryNs = r[i].timestampNs;
            entryPrice = r[i].price;
        }
    }
    return out;
}

static void testGenerated(const fs::path& tmp) {
    const fs::path raw = tmp / "gen_raw";
    CHECK(runCmd(std::string(GEN_BIN) + " --config " + CONFIG_PATH + " --out " + raw.string() +
                 " --seed 20250101 --start 2025-01-02 --end 2025-01-08 --limit 3") == 0, "gen failed");
    const fs::path store = tmp / "gen_store";
    ingest(raw, store, 7);
    Store s(store.string());

    // Strategy vs the naive implementation, segment by segment, on real generated data.
    const StrategyParams sets[] = {{20, 1.0, 0.0, 50}, {60, 1.5, 0.5, 300}, {1800, 2.0, 0.0, 3600}, {300, 1.0, -0.5, 100000}};
    size_t compared = 0, withTrades = 0;
    for (uint64_t id : {0, 1, 2, 1000, 1001, 1500, 1501, 1502}) {
        for (const Segment& seg : s.segments(id)) {
            const Record* r = s.records(id) + seg.begin;
            const size_t n = seg.end - seg.begin;
            for (const StrategyParams& p : sets) {
                std::vector<StrategyTrade> got;
                strategyOnSegment(r, n, p, got);
                const auto want = naiveStrategy(r, n, p);
                ++compared;
                withTrades += !want.empty();
                CHECK(got.size() == want.size(), "id " << id << " day " << seg.day << " W=" << p.window << ": " << got.size() << " vs " << want.size());
                for (size_t k = 0; k < std::min(got.size(), want.size()); ++k) {
                    CHECK(got[k].entryNs == want[k].entryNs && got[k].exitNs == want[k].exitNs && got[k].side == want[k].side &&
                              got[k].quantity == want[k].quantity && std::abs(got[k].pnl - want[k].pnl) < 1e-6,
                          "id " << id << " trade " << k);
                }
            }
        }
    }
    CHECK(compared > 100 && withTrades > 20, "cross-check should exercise trades: " << compared << " " << withTrades);

    // Trades: block-index MAE equals a straight scan, and both equal a brute-force reference.
    std::mt19937_64 rng(7);
    const int ids[] = {0, 1, 2, 1000, 1001, 1500, 1501, 1502, 3, 999, 1400};
    std::uniform_int_distribution<int64_t> when(ns(20090, 0), ns(20097, 0));
    std::uniform_int_distribution<int64_t> hold(0, 3LL * 86400 * 1'000'000'000LL);
    for (int i = 0; i < 4000; ++i) {
        TradeIn t;
        t.instrument = ids[rng() % 11];
        t.side = rng() % 2 ? 1 : -1;
        t.quantity = 1 + rng() % 500;
        t.entryNs = when(rng);
        t.exitNs = t.entryNs + (i % 4 == 0 ? static_cast<int64_t>(rng() % 5'000'000'000ULL) : hold(rng));
        const TradeOut a = priceTrade(s, t, MaeMode::Blocks), b = priceTrade(s, t, MaeMode::Scan);
        CHECK(a.ok == b.ok && (!a.ok || (a.mae == b.mae && a.pnl == b.pnl && a.entryNs == b.entryNs && a.exitNs == b.exitNs)),
              "blocks vs scan, trade " << i);

        // Reference: linear scan of the records.
        const uint64_t id = t.instrument;
        const Record* r = s.records(id);
        const uint64_t n = s.count(id);
        int64_t first = -1, last = -1;
        for (uint64_t k = 0; k < n; ++k) {
            if (first < 0 && r[k].timestampNs >= t.entryNs) first = k;
            if (r[k].timestampNs <= t.exitNs) last = k;
        }
        const bool ok = first >= 0 && last >= first;
        CHECK(a.ok == ok, "resolved? trade " << i);
        if (ok && a.ok) {
            double worst = 0;
            for (int64_t k = first; k <= last; ++k)
                worst = std::max(worst, -t.side * t.quantity * (static_cast<double>(r[k].price) - r[first].price));
            CHECK(std::abs(a.mae - worst) < 1e-9 && a.entryNs == r[first].timestampNs && a.exitNs == r[last].timestampNs,
                  "reference mae trade " << i << ": " << a.mae << " vs " << worst);
        }
    }
}

int main() {
    const fs::path tmp = fs::path(TEST_TMP_DIR) / "sim";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    testTradesHand(tmp);
    testSnapshotHand(tmp);
    testStrategyHand();
    testStrategyCli(tmp);
    testGenerated(tmp);
    if (failures == 0) std::cout << "all sim tests passed\n";
    return failures == 0 ? 0 : 1;
}

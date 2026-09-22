// sim strategy: per-instrument mean reversion on a trailing window of recorded
// seconds, reset at every segment (spec 5.3).
//
// Assumptions where the spec is open (also listed in README):
//  * The window is the last W *recorded* prices, including the current one. A
//    dropped second does not create a slot, so a window covers slightly more
//    than W wall-clock seconds. Signals start once W prices are in the segment.
//  * Max hold is wall-clock: exit when ts - entry_ts >= max_hold seconds.
//  * z uses population std; a window with no dispersion gives no signal, and an
//    open position can then still leave through max hold or the segment end.
//  * No position is opened on the last recorded second of a segment (it would be
//    closed in the same second at the same price).
//  * Sharpe uses every local trading day of the region (days without exits count as 0).
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

#include "sim_cmds.h"

namespace sim {

void strategyOnSegment(const Record* r, size_t n, const StrategyParams& p, std::vector<StrategyTrade>& out) {
    if (n == 0 || p.window < 1) return;
    const size_t W = static_cast<size_t>(p.window);
    const int64_t maxHoldNs = p.maxHoldSec * 1'000'000'000LL;

    // Prices are shifted by the segment's first price so the running sums stay small
    // and exactly-constant windows give exactly zero variance.
    const double base = r[0].price;
    thread_local std::vector<double> ring;
    ring.assign(W, 0.0);
    double sum = 0, sumSq = 0;
    size_t slot = 0;

    bool open = false;
    int side = 0;
    int64_t qty = 0, entryNs = 0;
    float entryPrice = 0;

    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(r[i].price) - base;
        if (i >= W) {
            const double old = ring[slot];
            sum -= old;
            sumSq -= old * old;
        }
        ring[slot] = x;
        sum += x;
        sumSq += x * x;
        if (++slot == W) slot = 0;

        bool signal = false;
        double z = 0;
        if (i + 1 >= W) {
            const double mean = sum / static_cast<double>(W);
            const double var = sumSq / static_cast<double>(W) - mean * mean;
            // Relative floor: below this the window is constant up to rounding.
            if (var > 1e-14 * base * base) {
                z = (x - mean) / std::sqrt(var);
                signal = true;
            }
        }
        const bool last = i + 1 == n;

        if (open) {
            const bool zExit = signal && (side > 0 ? z >= p.exit : z <= -p.exit);
            const bool held = r[i].timestampNs - entryNs >= maxHoldNs;
            if (zExit || held || last) {
                const float exitPrice = r[i].price;
                out.push_back({entryNs, r[i].timestampNs, entryPrice, exitPrice, qty, side,
                               side * static_cast<double>(qty) *
                                   (static_cast<double>(exitPrice) - static_cast<double>(entryPrice)),
                               0});
                open = false;  // no re-entry in the exit second
            }
        } else if (signal && !last) {
            if (z > p.entry) side = -1;
            else if (z < -p.entry) side = 1;
            else continue;
            qty = static_cast<int64_t>(std::floor(10000.0 / r[i].price));
            if (qty < 1) continue;
            open = true;
            entryNs = r[i].timestampNs;
            entryPrice = r[i].price;
        }
    }
}

namespace {

std::vector<uint8_t> loadReverters(const std::string& path, uint64_t nInstruments) {
    std::vector<uint8_t> flags(nInstruments, 0);
    const std::string text = readFile(path);
    std::string_view rest = text;
    while (!rest.empty()) {
        const size_t nl = rest.find('\n');
        std::string_view row = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
        while (!row.empty() && (row.back() == '\r' || row.back() == ' ')) row.remove_suffix(1);
        if (row.empty()) continue;
        uint64_t id = 0;
        const auto res = std::from_chars(row.data(), row.data() + row.size(), id);
        if (res.ec != std::errc() || res.ptr != row.data() + row.size())
            throw std::runtime_error("reverters file: bad line '" + std::string(row) + "'");
        if (id < nInstruments) flags[id] = 1;
    }
    return flags;
}

// Runs every segment of one instrument, reading each with pread into a per-thread buffer.
void runInstrument(const Store& store, uint64_t id, const StrategyParams& p, std::vector<StrategyTrade>& out) {
    thread_local std::vector<Record> buf;
    for (const Segment& seg : store.segments(id)) {
        const uint64_t n = seg.end - seg.begin;
        if (buf.size() < n) buf.resize(n);
        store.readRecords(id, seg.begin, n, buf.data());
        const size_t before = out.size();
        strategyOnSegment(buf.data(), n, p, out);
        for (size_t k = before; k < out.size(); ++k) out[k].exitDay = seg.day;
    }
}

struct Group {
    uint64_t trades = 0;
    double totalPnl = 0;
    std::unordered_map<int32_t, double> dailyPnl;  // by local exit date
};

double sharpe(const Group& g, const std::set<int32_t>& days) {
    if (days.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> daily;
    for (int32_t d : days) {
        auto it = g.dailyPnl.find(d);
        daily.push_back(it == g.dailyPnl.end() ? 0.0 : it->second);
    }
    double mean = 0;
    for (double v : daily) mean += v;
    mean /= static_cast<double>(daily.size());
    double ss = 0;
    for (double v : daily) ss += (v - mean) * (v - mean);
    const double sd = std::sqrt(ss / static_cast<double>(daily.size() - 1));
    if (sd == 0) return std::numeric_limits<double>::quiet_NaN();
    return mean / sd * std::sqrt(252.0);
}

void appendMaybeNan(std::string& out, double v) {
    if (std::isnan(v)) out += "nan";
    else appendNumber(out, v);
}

}  // namespace

RunInfo cmdStrategy(Args& args) {
    const std::string storeDir = args.required("store");
    const std::string revPath = args.required("reverters");
    StrategyParams p;
    p.window = static_cast<int64_t>(args.number("window"));
    p.entry = args.number("entry");
    p.exit = args.number("exit");
    p.maxHoldSec = static_cast<int64_t>(args.number("max-hold"));
    const std::string outPath = args.required("out");
    const std::string summaryPath = args.required("summary");
    const unsigned threads = static_cast<unsigned>(args.number("threads", defaultThreads()));
    args.check();
    if (p.window < 1) throw std::runtime_error("--window must be at least 1");
    if (p.maxHoldSec < 0) throw std::runtime_error("--max-hold must not be negative");

    Stopwatch clock;
    const Store store(storeDir);
    const uint64_t n = store.nInstruments();
    const std::vector<uint8_t> reverter = loadReverters(revPath, n);
    const uint32_t nRegions = store.nRegions();

    std::vector<std::set<int32_t>> regionDays(nRegions);
    for (uint64_t id = 0; id < n; ++id) {
        const int r = store.regionOf(id);
        if (r < 0) continue;
        for (const Segment& seg : store.segments(id)) regionDays[r].insert(seg.day);
    }

    std::FILE* tradesFile = std::fopen(outPath.c_str(), "wb");
    if (!tradesFile) throw std::runtime_error("cannot open " + outPath);
    const std::string header = "instrument_id,side,quantity,entry_ts_ns,exit_ts_ns,entry_price,exit_price,pnl\n";
    std::fwrite(header.data(), 1, header.size(), tradesFile);

    // Instruments finish out of order; results are emitted strictly in id order so the
    // output (and the float sums in the summary) do not depend on thread timing.
    std::vector<Group> groups(static_cast<size_t>(nRegions) * 2);
    std::vector<std::optional<std::vector<StrategyTrade>>> finished(n);
    std::mutex m;
    uint64_t nextEmit = 0, totalTrades = 0;
    std::string pending;
    std::exception_ptr error;

    auto emit = [&](uint64_t id, const std::vector<StrategyTrade>& trades) {
        const int region = store.regionOf(id);
        Group* g = region < 0 ? nullptr : &groups[region * 2 + reverter[id]];
        for (const StrategyTrade& t : trades) {
            appendNumber(pending, static_cast<int64_t>(id));
            pending += ',';
            appendNumber(pending, static_cast<int64_t>(t.side));
            pending += ',';
            appendNumber(pending, t.quantity);
            pending += ',';
            appendNumber(pending, t.entryNs);
            pending += ',';
            appendNumber(pending, t.exitNs);
            pending += ',';
            appendNumber(pending, t.entryPrice);
            pending += ',';
            appendNumber(pending, t.exitPrice);
            pending += ',';
            appendNumber(pending, t.pnl);
            pending += '\n';
            if (g) {
                ++g->trades;
                g->totalPnl += t.pnl;
                g->dailyPnl[t.exitDay] += t.pnl;
            }
        }
        totalTrades += trades.size();
        if (pending.size() > (4u << 20)) {
            std::fwrite(pending.data(), 1, pending.size(), tradesFile);
            pending.clear();
        }
    };

    ThreadPool pool(threads);
    pool.run(n, 1, [&](size_t begin, size_t end) {
        for (size_t id = begin; id < end; ++id) {
            std::vector<StrategyTrade> trades;
            try {
                runInstrument(store, id, p, trades);
            } catch (...) {
                std::lock_guard<std::mutex> lock(m);
                if (!error) error = std::current_exception();
            }
            std::lock_guard<std::mutex> lock(m);
            finished[id] = std::move(trades);
            while (nextEmit < n && finished[nextEmit]) {
                emit(nextEmit, *finished[nextEmit]);
                finished[nextEmit].reset();
                ++nextEmit;
            }
        }
    });
    if (error) std::rethrow_exception(error);
    std::fwrite(pending.data(), 1, pending.size(), tradesFile);
    if (std::fclose(tradesFile) != 0) throw std::runtime_error("failed to write " + outPath);

    std::string summary = "region,reverter_flag,trade_count,total_pnl,sharpe\n";
    std::string summaryJson = "[";
    for (uint32_t r = 0; r < nRegions; ++r) {
        for (int flag = 0; flag < 2; ++flag) {
            const Group& g = groups[r * 2 + flag];
            const double sh = sharpe(g, regionDays[r]);
            std::string row = store.regionName(r) + "," + std::to_string(flag) + "," +
                              std::to_string(g.trades) + ",";
            appendNumber(row, g.totalPnl);
            row += ',';
            appendMaybeNan(row, sh);
            summary += row + "\n";

            std::string js = "{\"region\":\"" + jsonEscape(store.regionName(r)) + "\",\"reverter\":" +
                             std::to_string(flag) + ",\"trades\":" + std::to_string(g.trades) + ",\"total_pnl\":";
            appendNumber(js, g.totalPnl);
            js += ",\"sharpe\":";
            if (std::isnan(sh)) js += "null";
            else appendNumber(js, sh);
            js += "}";
            summaryJson += (summaryJson.size() > 1 ? "," : "") + js;
        }
    }
    summaryJson += "]";
    writeFile(summaryPath, summary);

    std::cerr << "strategy: " << totalTrades << " trades in " << clock.seconds() << "s\n" << summary;
    RunInfo info;
    info.resultJson = "\"trades\":" + std::to_string(totalTrades) + ",\"summary\":" + summaryJson;
    return info;
}

}  // namespace sim

// sim trades: price a CSV of positions against the store.
#include <charconv>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>

#include "sim_cmds.h"

namespace sim {

TradeOut priceTrade(const Store& store, const TradeIn& t, MaeMode mode) {
    TradeOut out;
    if (t.instrument < 0 || static_cast<uint64_t>(t.instrument) >= store.nInstruments()) return out;
    const uint64_t id = static_cast<uint64_t>(t.instrument);

    const int64_t first = store.firstAtOrAfter(id, t.entryNs);
    const int64_t last = store.lastAtOrBefore(id, t.exitNs);
    if (first < 0 || last < 0 || last < first) return out;

    const Record* r = store.records(id);
    out.ok = true;
    out.entryNs = r[first].timestampNs;
    out.exitNs = r[last].timestampNs;
    out.entryPrice = r[first].price;
    out.exitPrice = r[last].price;
    out.pnl = t.side * t.quantity * (static_cast<double>(out.exitPrice) - static_cast<double>(out.entryPrice));

    // The worst unrealised loss needs only the lowest price for a long, the highest for a short.
    float lo, hi;
    if (mode == MaeMode::Blocks) {
        store.priceExtrema(id, first, last + 1, lo, hi);
    } else {
        lo = hi = r[first].price;
        for (int64_t i = first + 1; i <= last; ++i) {
            lo = std::min(lo, r[i].price);
            hi = std::max(hi, r[i].price);
        }
    }
    const double entry = out.entryPrice;
    const double loss = t.side > 0 ? entry - static_cast<double>(lo) : static_cast<double>(hi) - entry;
    out.mae = std::max(0.0, loss) * t.quantity;
    return out;
}

namespace {

template <typename T>
T parseField(std::string_view f, size_t line) {
    T v{};
    const auto res = std::from_chars(f.data(), f.data() + f.size(), v);
    if (res.ec != std::errc() || res.ptr != f.data() + f.size())
        throw std::runtime_error("trades csv line " + std::to_string(line) + ": bad number '" +
                                 std::string(f) + "'");
    return v;
}

}  // namespace

std::vector<TradeIn> parseTrades(const std::string& text) {
    std::vector<TradeIn> trades;
    std::string_view rest = text;
    size_t line = 0;
    while (!rest.empty()) {
        const size_t nl = rest.find('\n');
        std::string_view row = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
        ++line;
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        if (row.empty()) continue;
        if (line == 1 && std::isalpha(static_cast<unsigned char>(row[0]))) continue;  // header

        std::string_view f[6];
        for (int i = 0; i < 6; ++i) {
            const size_t comma = row.find(',');
            if ((comma == std::string_view::npos) != (i == 5))
                throw std::runtime_error("trades csv line " + std::to_string(line) + ": expected 6 fields");
            f[i] = row.substr(0, comma);
            if (i < 5) row.remove_prefix(comma + 1);
        }
        TradeIn t;
        t.id = f[0];
        t.instrument = parseField<int64_t>(f[1], line);
        t.side = static_cast<int>(parseField<int64_t>(f[2], line));
        t.quantity = parseField<int64_t>(f[3], line);
        t.entryNs = parseField<int64_t>(f[4], line);
        t.exitNs = parseField<int64_t>(f[5], line);
        trades.push_back(t);
    }
    return trades;
}

void appendTradeRow(std::string& out, const TradeIn& in, const TradeOut& r) {
    out.append(in.id);
    if (!r.ok) {
        out += ",1,,,,,,\n";
        return;
    }
    out += ",0,";
    appendNumber(out, r.entryNs);
    out += ',';
    appendNumber(out, r.exitNs);
    out += ',';
    appendNumber(out, r.entryPrice);
    out += ',';
    appendNumber(out, r.exitPrice);
    out += ',';
    appendNumber(out, r.pnl);
    out += ',';
    appendNumber(out, r.mae);
    out += '\n';
}

RunInfo cmdTrades(Args& args) {
    const std::string storeDir = args.required("store");
    const std::string inPath = args.required("in");
    const std::string outPath = args.required("out");
    const unsigned threads = static_cast<unsigned>(args.number("threads", defaultThreads()));
    const std::string maeName = args.str("mae", "blocks");
    args.check();
    if (maeName != "blocks" && maeName != "scan") throw std::runtime_error("--mae must be blocks or scan");
    const MaeMode mode = maeName == "scan" ? MaeMode::Scan : MaeMode::Blocks;

    Stopwatch clock;
    const Store store(storeDir);
    const std::string text = readFile(inPath);
    const std::vector<TradeIn> trades = parseTrades(text);
    std::cerr << "loaded " << trades.size() << " trades in " << clock.seconds() << "s\n";

    constexpr size_t kRowsPerChunk = 2048;
    const size_t nChunks = (trades.size() + kRowsPerChunk - 1) / kRowsPerChunk;
    std::vector<std::string> chunks(nChunks);
    std::atomic<uint64_t> unresolved{0};
    ThreadPool pool(threads);
    pool.run(nChunks, 1, [&](size_t c0, size_t c1) {
        for (size_t c = c0; c < c1; ++c) {
            std::string& out = chunks[c];
            out.reserve(kRowsPerChunk * 100);
            uint64_t bad = 0;
            const size_t end = std::min(trades.size(), (c + 1) * kRowsPerChunk);
            for (size_t i = c * kRowsPerChunk; i < end; ++i) {
                const TradeOut r = priceTrade(store, trades[i], mode);
                bad += !r.ok;
                appendTradeRow(out, trades[i], r);
            }
            unresolved += bad;
        }
    });

    std::string all = "trade_id,status,entry_ts_ns,exit_ts_ns,entry_price,exit_price,pnl,max_adverse_excursion\n";
    for (const std::string& c : chunks) all += c;
    writeFile(outPath, all);
    std::cerr << "priced " << trades.size() << " trades (" << unresolved << " unresolved) in "
              << clock.seconds() << "s\n";

    RunInfo info;
    info.resultJson = "\"trades\":" + std::to_string(trades.size()) +
                      ",\"unresolved\":" + std::to_string(unresolved.load());
    return info;
}

}  // namespace sim

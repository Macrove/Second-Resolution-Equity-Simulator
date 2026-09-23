// The three sim subcommands. Each parses its own options from args, does the work,
// and reports what the run log needs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sim_common.h"
#include "store.h"

namespace sim {

// What a subcommand hands back for the run log: result fields as JSON members
// ("\"k\":v,\"k2\":v2", may be empty). Options are logged from the command line.
struct RunInfo {
    std::string resultJson;
};

// ---- trades (5.1) ----------------------------------------------------------

struct TradeIn {
    std::string_view id;  // text of trade_id, points into the input buffer
    int64_t instrument = 0;
    int side = 0;
    int64_t quantity = 0;
    int64_t entryNs = 0;
    int64_t exitNs = 0;
};

struct TradeOut {
    bool ok = false;  // false = status 1, no numbers
    int64_t entryNs = 0, exitNs = 0;  // snapped timestamps
    float entryPrice = 0, exitPrice = 0;
    double pnl = 0;
    double mae = 0;  // max adverse excursion, in the same money units as pnl
};

enum class MaeMode { Blocks, Scan };  // Scan is the plain read-every-record baseline

// Entry snaps forward, exit snaps backward; status 1 if either has no record or
// exit < entry after snapping. MAE is the largest unrealised loss (floored at 0)
// over every recorded second in [entry, exit], scaled by quantity like pnl.
TradeOut priceTrade(const Store& store, const TradeIn& t, MaeMode mode);

std::vector<TradeIn> parseTrades(const std::string& text);
void appendTradeRow(std::string& out, const TradeIn& in, const TradeOut& r);
RunInfo cmdTrades(Args& args);

// ---- snapshot (5.2) --------------------------------------------------------

constexpr size_t kSnapshotWidth = 2000;
// row[i] = last known price of instrument i at ts, NaN if none (kSnapshotWidth entries).
void snapshotRow(const Store& store, int64_t ts, float* row, ThreadPool& pool);
RunInfo cmdSnapshot(Args& args);

// ---- strategy (5.3) --------------------------------------------------------

struct StrategyParams {
    int64_t window = 0;    // recorded seconds (records) in the trailing window
    double entry = 0;
    double exit = 0;
    int64_t maxHoldSec = 0;
};

struct StrategyTrade {
    int64_t entryNs, exitNs;
    float entryPrice, exitPrice;
    int64_t quantity;
    int side;  // +1 long, -1 short
    double pnl;
    int32_t exitDay;  // local trading date of the exit; set by the caller that knows the segment
};

// Runs the strategy over one segment's records (time-sorted). Window state and any
// open position live only inside the segment: whatever is open closes at its last record.
void strategyOnSegment(const Record* r, size_t n, const StrategyParams& p, std::vector<StrategyTrade>& out);
RunInfo cmdStrategy(Args& args);

}  // namespace sim

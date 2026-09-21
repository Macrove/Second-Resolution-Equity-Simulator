// Config extracted once from region.yaml into plain structs, plus date and
// timezone helpers shared by the generator and its tests.
#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace sim {

constexpr int64_t kSecondsPerDay = 86400;

// Days since 1970-01-01 for a civil date.
int64_t daysFromCivil(int year, unsigned month, unsigned day);
void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day);

// Accepts "YYYY-MM-DD" or "YYYY.MM.DD". Throws std::runtime_error if malformed.
int64_t parseDate(const std::string& text);
std::string formatDate(int64_t days);  // "YYYYMMDD"

// 0 = Sunday ... 6 = Saturday (config uses 1-5 for Mon-Fri).
int dayOfWeek(int64_t days);

// Floor division/modulo that behave for negative numerators.
int64_t floorDiv(int64_t a, int64_t b);

// A continuous block of trading, in local seconds since midnight: [start, end).
struct Session {
    int64_t startSec = 0;
    int64_t endSec = 0;
};

struct Range {
    double lo = 0;
    double hi = 0;
};

struct Region {
    std::string name;
    int id = 0;
    std::string localTimezone;
    std::vector<Session> sessions;
    int nStocks = 0;
    int firstSecurityId = 0;  // cumulative NStocks of the regions before this one
    std::array<uint8_t, 7> tradeDayOfWeek{};  // indexed by dayOfWeek(); 1 = trades
    bool hasDst = false;
    int64_t dstRangeStart = 0;  // day number; only meaningful if hasDst
    int64_t utc2localOffsetSec = 0;
    int64_t utc2localDstOffsetSec = 0;
    std::set<int64_t> holidays;  // day numbers
};

struct Config {
    std::vector<Region> regions;
    // Run period (day numbers). loadConfig fills these from DefaultStartDate /
    // DefaultEndDate; command-line arguments override them.
    int64_t startDay = 0;
    int64_t endDay = 0;

    double dropProbability = 0;  // per second, fraction
    Range startPrice;
    Range annualVol;       // fractions (0.15 = 15%)
    Range baseVolume;
    double revertorProbability = 0;  // fraction
    Range halfLifeSecs;
    Range ouLongRunStd;    // fractions
    double perSecVolDenominatorSquared = 0;  // 252 * 23400
};

Config loadConfig(const std::string& path);

// Local wall clock and UTC are both expressed as seconds since the epoch;
// a "local epoch" value is the wall clock read as if it were UTC. The DST
// offset applies from dstRangeStart (00:00 local) onward. The end of DST is
// ignored because it lies outside the simulated period.
int64_t local2utc(const Region& region, int64_t localEpochSec);
int64_t utc2local(const Region& region, int64_t utcEpochSec);

bool isHoliday(const Region& region, int64_t day);
bool isTradingDay(const Region& region, int64_t day);

}  // namespace sim

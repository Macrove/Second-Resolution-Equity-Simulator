#include "config.h"

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace sim {

int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    using namespace std::chrono;
    return sys_days{year{y} / month{m} / day{d}}.time_since_epoch().count();
}

void civilFromDays(int64_t dayNum, int& y, unsigned& m, unsigned& d) {
    using namespace std::chrono;
    const year_month_day ymd{sys_days{std::chrono::days{dayNum}}};
    y = static_cast<int>(ymd.year());
    m = static_cast<unsigned>(ymd.month());
    d = static_cast<unsigned>(ymd.day());
}

int64_t parseDate(const std::string& text) {
    int y = 0, m = 0, d = 0;
    char s1 = 0, s2 = 0;
    int consumed = 0;
    if (std::sscanf(text.c_str(), "%d%c%d%c%d%n", &y, &s1, &m, &s2, &d, &consumed) != 5 ||
        consumed != static_cast<int>(text.size()) || (s1 != '-' && s1 != '.') || s2 != s1) {
        throw std::runtime_error("bad date '" + text + "' (expected YYYY-MM-DD or YYYY.MM.DD)");
    }
    using namespace std::chrono;
    const year_month_day ymd{year{y} / month{static_cast<unsigned>(m)} / day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) throw std::runtime_error("date does not exist: '" + text + "'");
    return sys_days{ymd}.time_since_epoch().count();
}

std::string formatDate(int64_t days) {
    int y;
    unsigned m, d;
    civilFromDays(days, y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d%02u%02u", y, m, d);
    return buf;
}

int dayOfWeek(int64_t dayNum) {
    using namespace std::chrono;
    return static_cast<int>(weekday{sys_days{std::chrono::days{dayNum}}}.c_encoding());
}

int64_t local2utc(const Region& region, int64_t localEpochSec) {
    const int64_t day = floorDiv(localEpochSec, kSecondsPerDay);
    const bool dst = region.hasDst && day >= region.dstRangeStart;
    return localEpochSec - (dst ? region.utc2localDstOffsetSec : region.utc2localOffsetSec);
}

int64_t utc2local(const Region& region, int64_t utcEpochSec) {
    // Decide DST from the local date; probing with the standard offset is
    // exact except within hours of the changeover, when nothing trades.
    const int64_t day = floorDiv(utcEpochSec + region.utc2localOffsetSec, kSecondsPerDay);
    const bool dst = region.hasDst && day >= region.dstRangeStart;
    return utcEpochSec + (dst ? region.utc2localDstOffsetSec : region.utc2localOffsetSec);
}

bool isHoliday(const Region& region, int64_t day) { return region.holidays.count(day) > 0; }

bool isTradingDay(const Region& region, int64_t day) {
    return region.tradeDayOfWeek[dayOfWeek(day)] && !isHoliday(region, day);
}

namespace {

int64_t parseTimeOfDay(const std::string& text) {
    int h = 0, m = 0, consumed = 0;
    if (std::sscanf(text.c_str(), "%d:%d%n", &h, &m, &consumed) != 2 ||
        consumed != static_cast<int>(text.size()) || h < 0 || h > 24 || m < 0 || m > 59) {
        throw std::runtime_error("bad time '" + text + "' (expected HH:MM)");
    }
    return h * 3600 + m * 60;
}

// Top-level entries are single-key maps ("- Key: value"); find one by key.
YAML::Node findEntry(const YAML::Node& root, const std::string& key) {
    for (const auto& entry : root) {
        const YAML::Node value = entry[key];
        if (value) return value;
    }
    throw std::runtime_error("region.yaml: missing entry '" + key + "'");
}

Range asRange(const YAML::Node& node, double scale) {
    if (!node.IsSequence() || node.size() != 2) {
        throw std::runtime_error("region.yaml: expected a [lo, hi] pair");
    }
    return {node[0].as<double>() * scale, node[1].as<double>() * scale};
}

Region parseRegion(const YAML::Node& node) {
    Region r;
    r.name = node["name"].as<std::string>();
    r.id = node["id"].as<int>();
    r.localTimezone = node["localTimezone"].as<std::string>();
    r.nStocks = node["NStocks"].as<int>();
    r.utc2localOffsetSec = node["UTC2LocalOffsetHours"].as<int>() * 3600;
    r.utc2localDstOffsetSec = node["UTC2LocalDSTOffsetHours"].as<int>() * 3600;

    const YAML::Node starts = node["LocalStartTimes"];
    const YAML::Node ends = node["LocalEndTimes"];
    if (!starts || !ends || starts.size() != ends.size() || starts.size() == 0) {
        throw std::runtime_error("region " + r.name + ": LocalStartTimes/LocalEndTimes mismatch");
    }
    for (size_t i = 0; i < starts.size(); ++i) {
        Session s{parseTimeOfDay(starts[i].as<std::string>()),
                  parseTimeOfDay(ends[i].as<std::string>())};
        if (s.endSec <= s.startSec || (i > 0 && s.startSec < r.sessions.back().endSec)) {
            throw std::runtime_error("region " + r.name + ": sessions must be ordered and non-empty");
        }
        r.sessions.push_back(s);
    }

    for (const auto& d : node["TradeDayOfWeek"]) {
        const int dow = d.as<int>();
        if (dow < 0 || dow > 6) throw std::runtime_error("region " + r.name + ": bad TradeDayOfWeek");
        r.tradeDayOfWeek[dow] = 1;
    }

    if (const YAML::Node dst = node["DSTRangeStart"]) {
        r.hasDst = true;
        r.dstRangeStart = parseDate(dst[0].as<std::string>());
    }
    if (const YAML::Node holidays = node["Holidays"]) {
        for (const auto& h : holidays) r.holidays.insert(parseDate(h.as<std::string>()));
    }
    return r;
}

}  // namespace

Config loadConfig(const std::string& path) {
    const YAML::Node root = YAML::LoadFile(path);
    Config cfg;

    int nextId = 0;
    for (const auto& regionNode : findEntry(root, "Region")) {
        Region r = parseRegion(regionNode);
        r.firstSecurityId = nextId;
        nextId += r.nStocks;
        cfg.regions.push_back(std::move(r));
    }

    cfg.startDay = parseDate(findEntry(root, "DefaultStartDate").as<std::string>());
    cfg.endDay = parseDate(findEntry(root, "DefaultEndDate").as<std::string>());
    cfg.dropProbability = findEntry(root, "TradeSecondDropProbabilityPct").as<double>() / 100;
    cfg.startPrice = asRange(findEntry(root, "StartPriceUniform"), 1);
    cfg.annualVol = asRange(findEntry(root, "AnnualVolatilityUniformPct"), 0.01);
    cfg.baseVolume = asRange(findEntry(root, "BaseVolumeUniform"), 1);
    cfg.revertorProbability = findEntry(root, "RegionIsRevertorProbabilityPct").as<double>() / 100;
    cfg.halfLifeSecs = asRange(findEntry(root, "HalfLifeSecsUniform"), 1);
    cfg.ouLongRunStd = asRange(findEntry(root, "OULongRunStdUniformPct"), 0.01);
    cfg.perSecVolDenominatorSquared =
        findEntry(root, "PerSecondVolatilityDenominatorSquared").as<double>();
    return cfg;
}

}  // namespace sim

// Synthetic one-second price generator.
//
//   ./gen --out raw/ --seed 20250101 --start 2025-01-02 --end 2025-03-31
//
// For each region, for each stock: draw the per-instrument parameters once,
// then walk the dates. Every trading day is generated as one in-memory buffer
// and handed to std::async for writing to <out>/YYYYMMDD/NNNN.bin.
//
// Each instrument has its own RNG seeded from (seed, security id), so output
// depends only on the seed, never on iteration order or IO timing.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"

namespace {

using namespace sim;

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "records are little-endian on disk");

struct Record {
    int64_t timestampNs;
    float price;
    int32_t volume;
};
static_assert(sizeof(Record) == 16, "record must be 16 bytes");

constexpr double kTradingDaysPerYear = 252;
constexpr double kOvernightJumpFactor = 0.5;
constexpr uint64_t kDefaultSeed = 20250101;

// std::async(launch::async) starts one thread per file, so this caps both the
// number of writer threads and the buffers held in memory (~0.5 MB each).
constexpr size_t kMaxInFlightWrites = 8;

struct Options {
    std::string out = "raw";
    std::string configPath = "region.yaml";
    uint64_t seed = kDefaultSeed;
    std::optional<int64_t> startDay;
    std::optional<int64_t> endDay;
    int limitPerRegion = -1;  // test aid: only the first N stocks of each region
};

void usage(const char* prog) {
    std::cerr << "usage: " << prog
              << " [--out DIR] [--seed N] [--start YYYY-MM-DD] [--end YYYY-MM-DD]\n"
                 "       [--config FILE] [--limit N_PER_REGION]\n";
}

Options parseArgs(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
            return argv[++i];
        };
        if (flag == "--out") opt.out = value();
        else if (flag == "--config") opt.configPath = value();
        else if (flag == "--seed") opt.seed = std::stoull(value());
        else if (flag == "--start") opt.startDay = parseDate(value());
        else if (flag == "--end") opt.endDay = parseDate(value());
        else if (flag == "--limit") opt.limitPerRegion = std::stoi(value());
        else if (flag == "--help" || flag == "-h") { usage(argv[0]); std::exit(0); }
        else throw std::runtime_error("unknown argument " + flag);
    }
    return opt;
}

// Everything drawn or derived once per instrument, plus the running walk.
struct Instrument {
    int id = 0;
    const Region* region = nullptr;
    std::mt19937_64 rng;

    double logStartPrice = 0;
    double annualVol = 0;
    double perSecVol = 0;
    double baseVolume = 0;
    bool reverter = false;
    double decay = 0;    // exp(-1/tau)
    double shockSd = 0;  // OU innovation sd giving the drawn long-run sd

    double walk = 0;         // random-walk log price, carried across segments and days
    bool hasTraded = false;  // false until the first segment starts (no jump before it)
};

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

Instrument drawInstrument(int id, const Region& region, const Config& cfg, uint64_t seed) {
    Instrument s;
    s.id = id;
    s.region = &region;
    s.rng.seed(splitmix64(seed ^ splitmix64(static_cast<uint64_t>(id) + 1)));

    auto uniform = [&](const Range& r) {
        return std::uniform_real_distribution<double>(r.lo, r.hi)(s.rng);
    };
    s.logStartPrice = std::log(uniform(cfg.startPrice));
    s.annualVol = uniform(cfg.annualVol);
    s.baseVolume = uniform(cfg.baseVolume);
    s.reverter = std::uniform_real_distribution<double>(0, 1)(s.rng) < cfg.revertorProbability;
    const double halfLife = uniform(cfg.halfLifeSecs);
    const double longRunStd = uniform(cfg.ouLongRunStd);

    s.perSecVol = s.annualVol / std::sqrt(cfg.perSecVolDenominatorSquared);
    if (s.reverter) {
        const double tau = halfLife / std::log(2.0);
        s.decay = std::exp(-1.0 / tau);
        s.shockSd = longRunStd * std::sqrt(1.0 - s.decay * s.decay);
    }
    return s;
}

// Appends one day of records for the instrument. The price process advances
// every second of every segment; dropped seconds only lose their record.
void generateDay(Instrument& s, const Config& cfg, int64_t day, std::vector<Record>& out) {
    const Region& region = *s.region;
    std::normal_distribution<double> normal;
    std::uniform_real_distribution<double> uniform01;
    std::poisson_distribution<int32_t> poisson;
    using PoissonParam = std::poisson_distribution<int32_t>::param_type;

    for (size_t k = 0; k < region.sessions.size(); ++k) {
        const Session& session = region.sessions[k];

        if (s.hasTraded) {
            // Overnight for the first segment of a day, otherwise the gap
            // (Tokyo lunch) between two segments of the same day.
            const double jumpSd =
                k == 0 ? s.annualVol / std::sqrt(kTradingDaysPerYear) * kOvernightJumpFactor
                       : s.perSecVol * std::sqrt(static_cast<double>(
                                           session.startSec - region.sessions[k - 1].endSec));
            s.walk += jumpSd * normal(s.rng);
        }
        s.hasTraded = true;
        double ou = 0;  // mean-reverting component resets every segment

        const int64_t startUtc = local2utc(region, day * kSecondsPerDay + session.startSec);
        const int64_t length = session.endSec - session.startSec;
        for (int64_t i = 0; i < length; ++i) {
            s.walk += s.perSecVol * normal(s.rng);
            if (s.reverter) ou = ou * s.decay + s.shockSd * normal(s.rng);

            if (uniform01(s.rng) < cfg.dropProbability) continue;

            const double u = static_cast<double>(i) / static_cast<double>(length);
            const double shape = 2.0 * u - 1.0;
            const double meanVolume = s.baseVolume * (0.5 + 2.0 * shape * shape);
            out.push_back({(startUtc + i) * 1'000'000'000LL,
                           static_cast<float>(std::exp(s.logStartPrice + s.walk + ou)),
                           std::max<int32_t>(1, poisson(s.rng, PoissonParam(meanVolume)))});
        }
    }
}

void writeFile(const std::string& path, const std::vector<Record>& records) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(records.data()),
               static_cast<std::streamsize>(records.size() * sizeof(Record)));
    file.close();
    if (!file) throw std::runtime_error("failed to write " + path);
}

// Writes files on background threads, blocking the producer once too many
// buffers are in flight so memory stays bounded.
class AsyncWriter {
public:
    explicit AsyncWriter(size_t maxInFlight) : maxInFlight_(maxInFlight) {}

    void submit(std::string path, std::vector<Record>&& records) {
        while (pending_.size() >= maxInFlight_) reapOldest();
        pending_.push_back(std::async(std::launch::async,
                                      [p = std::move(path), r = std::move(records)] {
                                          writeFile(p, r);
                                      }));
    }

    void drain() {
        while (!pending_.empty()) reapOldest();
    }

private:
    void reapOldest() {
        pending_.front().get();  // rethrows any IO error
        pending_.pop_front();
    }

    size_t maxInFlight_;
    std::deque<std::future<void>> pending_;
};

int run(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);
    Config cfg = loadConfig(opt.configPath);
    if (opt.startDay) cfg.startDay = *opt.startDay;
    if (opt.endDay) cfg.endDay = *opt.endDay;
    if (cfg.startDay > cfg.endDay) throw std::runtime_error("--start is after --end");

    const std::filesystem::path outDir = opt.out;
    std::filesystem::create_directories(outDir);
    std::set<int64_t> madeDirs;
    auto dayDir = [&](int64_t day) {
        const std::filesystem::path dir = outDir / formatDate(day);
        if (madeDirs.insert(day).second) std::filesystem::create_directories(dir);
        return dir;
    };

    AsyncWriter writer(kMaxInFlightWrites);
    std::vector<int> reverters;
    const auto t0 = std::chrono::steady_clock::now();

    for (const Region& region : cfg.regions) {
        const int nStocks =
            opt.limitPerRegion >= 0 ? std::min(region.nStocks, opt.limitPerRegion) : region.nStocks;
        for (int k = 0; k < nStocks; ++k) {
            const int id = region.firstSecurityId + k;
            Instrument stock = drawInstrument(id, region, cfg, opt.seed);
            if (stock.reverter) reverters.push_back(id);

            char name[16];
            std::snprintf(name, sizeof name, "%04d.bin", id);

            for (int64_t day = cfg.startDay; day <= cfg.endDay; ++day) {
                // Weekends and holidays produce no records, i.e. an empty file.
                std::vector<Record> records;
                if (isTradingDay(region, day)) {
                    size_t seconds = 0;
                    for (const Session& s : region.sessions) seconds += s.endSec - s.startSec;
                    records.reserve(seconds);
                    generateDay(stock, cfg, day, records);
                }
                writer.submit((dayDir(day) / name).string(), std::move(records));
            }

            if ((k + 1) % 100 == 0 || k + 1 == nStocks) {
                const double secs =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::cerr << region.name << ": " << (k + 1) << "/" << nStocks << " stocks, "
                          << secs << "s\n";
            }
        }
    }
    writer.drain();

    std::ofstream revFile(outDir / "reverters.txt", std::ios::trunc);
    for (int id : reverters) revFile << id << '\n';
    revFile.close();
    if (!revFile) throw std::runtime_error("failed to write reverters.txt");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        usage(argv[0]);
        return 2;
    }
}

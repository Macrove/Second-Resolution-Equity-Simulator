// End-to-end checks: runs the ./gen binary with small --limit runs and inspects
// the files it writes. Expected UTC session starts are hardcoded on purpose so
// a mistake in region.yaml offsets cannot cancel itself out.
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "config.h"

namespace fs = std::filesystem;
using namespace sim;

struct Record {
    int64_t timestampNs;
    float price;
    int32_t volume;
};
static_assert(sizeof(Record) == 16);

static int failures = 0;
#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++failures;                                                              \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond " -- " << msg << '\n'; \
        }                                                                            \
    } while (0)

static std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static std::vector<Record> readRecords(const fs::path& p) {
    const std::string bytes = readAll(p);
    std::vector<Record> r(bytes.size() / sizeof(Record));
    std::memcpy(r.data(), bytes.data(), r.size() * sizeof(Record));
    return r;
}

static fs::path fileFor(const fs::path& out, const std::string& date, int id) {
    char name[16];
    std::snprintf(name, sizeof name, "%04d.bin", id);
    return out / date / name;
}

static void runGen(const fs::path& out, const std::string& start, const std::string& end,
                   const std::string& seed = "20250101", int limit = 1) {
    const std::string cmd = std::string(GEN_BIN) + " --config " + CONFIG_PATH + " --out " +
                            out.string() + " --seed " + seed + " --start " + start + " --end " +
                            end + " --limit " + std::to_string(limit) + " 2>/dev/null";
    CHECK(std::system(cmd.c_str()) == 0, cmd);
}

static int64_t secOfDay(int64_t sec) { return ((sec % kSecondsPerDay) + kSecondsPerDay) % kSecondsPerDay; }

static void testEmptyDays(const Config& cfg, const fs::path& tmp) {
    struct Case { const char* date; std::vector<int> emptyIds, dataIds; };
    // 2025-01-09 US holiday, 2025-01-13 Japan holiday, 2025-01-04 Saturday.
    const Case cases[] = {{"2025-01-09", {0}, {1000, 1500}},
                          {"2025-01-13", {1500}, {0, 1000}},
                          {"2025-01-04", {0, 1000, 1500}, {}}};
    for (const Case& c : cases) {
        const fs::path out = tmp / (std::string("empty_") + c.date);
        runGen(out, c.date, c.date);
        const std::string dir = formatDate(parseDate(c.date));
        for (int id : c.emptyIds) {
            const fs::path f = fileFor(out, dir, id);
            CHECK(fs::exists(f) && fs::file_size(f) == 0, f << " should exist and be empty");
        }
        for (int id : c.dataIds) {
            const fs::path f = fileFor(out, dir, id);
            CHECK(fs::exists(f) && fs::file_size(f) > 0, f << " should have data");
        }
    }
    (void)cfg;
}

static void testSessionTimes(const Config& cfg, const fs::path& tmp) {
    // First UTC second of each session, hand-computed from real-world offsets.
    struct Case { const char* date; int64_t us, eu; };
    const Case cases[] = {
        {"2025-03-07", 14 * 3600 + 1800, 8 * 3600},  // US EST, Berlin CET
        {"2025-03-10", 13 * 3600 + 1800, 8 * 3600},  // US EDT (from 9 Mar), Berlin CET
        {"2025-03-28", 13 * 3600 + 1800, 8 * 3600},
        {"2025-03-31", 13 * 3600 + 1800, 7 * 3600},  // Berlin CEST (from 30 Mar)
    };
    const int64_t tokyoStarts[] = {0, 3 * 3600 + 1800};  // 09:00 and 12:30 JST

    for (const Case& c : cases) {
        const fs::path out = tmp / (std::string("times_") + c.date);
        runGen(out, c.date, c.date);
        const int64_t day = parseDate(c.date);
        const std::string dir = formatDate(day);

        for (const Region& region : cfg.regions) {
            const auto recs = readRecords(fileFor(out, dir, region.firstSecurityId));
            int64_t expected = 0;
            for (const Session& s : region.sessions) expected += s.endSec - s.startSec;
            CHECK(recs.size() > 0.98 * expected && recs.size() <= static_cast<size_t>(expected),
                  region.name << " " << c.date << " count " << recs.size() << " vs " << expected);

            std::vector<int64_t> first(region.sessions.size(), -1), last(region.sessions.size(), -1);
            int64_t prev = -1;
            for (const Record& r : recs) {
                CHECK(r.timestampNs % 1'000'000'000 == 0, "whole seconds");
                const int64_t utc = r.timestampNs / 1'000'000'000;
                CHECK(utc > prev, "strictly increasing timestamps");
                prev = utc;
                const int64_t local = utc2local(region, utc);
                CHECK(floorDiv(local, kSecondsPerDay) == day, "local date matches file date");
                const int64_t sod = secOfDay(local);
                bool inSession = false;
                for (size_t k = 0; k < region.sessions.size(); ++k) {
                    if (sod >= region.sessions[k].startSec && sod < region.sessions[k].endSec) {
                        inSession = true;
                        if (first[k] < 0) first[k] = sod;
                        last[k] = sod;
                    }
                }
                CHECK(inSession, region.name << " " << c.date << " record outside session");
            }
            // Config-derived: every segment starts/ends at its local time (a
            // few seconds of slack because ~0.5% of seconds are dropped).
            for (size_t k = 0; k < region.sessions.size(); ++k) {
                CHECK(first[k] >= region.sessions[k].startSec && first[k] < region.sessions[k].startSec + 10,
                      region.name << " " << c.date << " seg " << k << " starts at local " << first[k]);
                CHECK(last[k] < region.sessions[k].endSec && last[k] >= region.sessions[k].endSec - 10,
                      region.name << " " << c.date << " seg " << k << " ends at local " << last[k]);
            }
            // Hardcoded UTC.
            std::vector<int64_t> wantUtc;
            if (region.name == "US") wantUtc = {c.us};
            else if (region.name == "Europe") wantUtc = {c.eu};
            else wantUtc = {tokyoStarts[0], tokyoStarts[1]};
            // Locate each segment's first record in UTC.
            size_t seg = 0;
            int64_t lastUtcSod = -1000000;
            for (const Record& r : recs) {
                const int64_t sod = secOfDay(r.timestampNs / 1'000'000'000);
                if (sod - lastUtcSod > 600) {  // gap => new segment
                    CHECK(seg < wantUtc.size(), "unexpected extra segment");
                    if (seg < wantUtc.size()) {
                        CHECK(sod >= wantUtc[seg] && sod < wantUtc[seg] + 10,
                              region.name << " " << c.date << " seg " << seg << " first UTC sod " << sod
                                          << " want " << wantUtc[seg]);
                    }
                    ++seg;
                }
                lastUtcSod = sod;
            }
            CHECK(seg == wantUtc.size(), region.name << " segment count " << seg);
        }
    }
}

static void testDeterminism(const fs::path& tmp) {
    const fs::path a = tmp / "det_a", b = tmp / "det_b", c = tmp / "det_c";
    runGen(a, "2025-03-10", "2025-03-12", "20250101", 2);
    runGen(b, "2025-03-10", "2025-03-12", "20250101", 2);
    runGen(c, "2025-03-10", "2025-03-12", "1", 2);
    int compared = 0;
    for (const auto& e : fs::recursive_directory_iterator(a)) {
        if (!e.is_regular_file()) continue;
        const fs::path rel = fs::relative(e.path(), a);
        CHECK(readAll(e.path()) == readAll(b / rel), "same seed differs: " << rel);
        ++compared;
    }
    CHECK(compared >= 3 * 3 * 2, "compared " << compared << " files");
    CHECK(readAll(fileFor(a, "20250310", 0)) != readAll(fileFor(c, "20250310", 0)),
          "different seeds should differ");
}

int main() {
    const Config cfg = loadConfig(CONFIG_PATH);
    const fs::path tmp = TEST_TMP_DIR;
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    testEmptyDays(cfg, tmp);
    testSessionTimes(cfg, tmp);
    testDeterminism(tmp);

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all generator tests passed\n";
    return 0;
}

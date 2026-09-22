// Checks for ./ingest and store.h. A hand-built tiny dataset pins down exact
// answers; a generated week (with a weekend, a US holiday and missing ids) is
// compared against a linear scan of the raw files, with a tiny stride so
// bracket boundaries are hit constantly and with the default stride.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "store.h"
#include "test_util.h"

static int runIngest(const fs::path& raw, const fs::path& store, uint64_t stride) {
    return runCmd(std::string(INGEST_BIN) + " --raw " + raw.string() + " --store " + store.string() +
                  " --config " + CONFIG_PATH + " --stride " + std::to_string(stride));
}

static Record rec(int64_t ts) { return {ts, static_cast<float>(ts) / 10.0f, static_cast<int32_t>(ts)}; }

static void testHandBuilt(const fs::path& tmp) {
    const fs::path raw = tmp / "hand_raw";
    // Instrument 0: two days (5 records, second-day file holds 40, 50). Instrument 1: absent.
    // Instrument 2: present only on the second day. Day 3 is empty for everyone.
    writeRecords(fileFor(raw, "20250102", 0), {rec(10), rec(20), rec(30)});
    writeRecords(fileFor(raw, "20250103", 0), {rec(40), rec(50)});
    writeRecords(fileFor(raw, "20250103", 2), {rec(35)});
    writeRecords(fileFor(raw, "20250104", 0), {});
    writeRecords(fileFor(raw, "20250104", 2), {});

    for (uint64_t stride : {1, 2, 3, 256}) {
        const fs::path store = tmp / ("hand_store_" + std::to_string(stride));
        CHECK(runIngest(raw, store, stride) == 0, "ingest failed, stride " << stride);
        CHECK(!fs::exists(store / "data.db.tmp") && !fs::exists(store / "index.idx.tmp"), "tmp left");
        Store s(store.string());
        CHECK(s.nInstruments() == 3 && s.totalRecords() == 6, "stride " << stride);
        CHECK(s.count(0) == 5 && s.count(1) == 0 && s.count(2) == 1 && s.count(3) == 0, "counts");

        const struct { int64_t t, last, first; } cases[] = {
            {5, -1, 0}, {10, 0, 0}, {11, 0, 1}, {20, 1, 1}, {25, 1, 2},
            {30, 2, 2}, {35, 2, 3}, {40, 3, 3}, {50, 4, 4}, {51, 4, -1}, {99, 4, -1}};
        for (const auto& c : cases) {
            CHECK(s.lastAtOrBefore(0, c.t) == c.last, "last t=" << c.t << " stride " << stride);
            CHECK(s.firstAtOrAfter(0, c.t) == c.first, "first t=" << c.t << " stride " << stride);
        }
        CHECK(s.lastAtOrBefore(2, 34) == -1 && s.lastAtOrBefore(2, 35) == 0 && s.lastAtOrBefore(2, 99) == 0, "id 2 last");
        CHECK(s.firstAtOrAfter(2, 35) == 0 && s.firstAtOrAfter(2, 36) == -1, "id 2 first");
        for (uint64_t id : {1, 3, 1999}) {
            CHECK(s.lastAtOrBefore(id, 30) == -1 && s.firstAtOrAfter(id, 30) == -1, "absent id " << id);
        }
    }
}

static void testCorruptInput(const fs::path& tmp) {
    {
        const fs::path raw = tmp / "bad_size_raw";
        writeRecords(fileFor(raw, "20250102", 0), {rec(10), rec(20)});
        const char junk[20] = {};
        writeBytes(fileFor(raw, "20250103", 0), junk, sizeof junk);
        const fs::path store = tmp / "bad_size_store";
        CHECK(runIngest(raw, store, 4) != 0, "size not a multiple of 16 must fail");
        CHECK(!fs::exists(store / "index.idx"), "failed ingest must not leave a store");
    }
    {
        const fs::path raw = tmp / "unsorted_raw";
        writeRecords(fileFor(raw, "20250102", 0), {rec(10), rec(20)});
        writeRecords(fileFor(raw, "20250103", 0), {rec(15), rec(30)});  // goes back across the join
        const fs::path store = tmp / "unsorted_store";
        CHECK(runIngest(raw, store, 4) != 0, "out-of-order timestamps must fail");
        CHECK(!fs::exists(store / "index.idx"), "failed ingest must not leave a store");
    }
}

static void testGenerated(const fs::path& tmp) {
    const fs::path raw = tmp / "gen_raw";
    CHECK(runCmd(std::string(GEN_BIN) + " --config " + CONFIG_PATH + " --out " + raw.string() +
                 " --seed 20250101 --start 2025-01-02 --end 2025-01-10 --limit 3") == 0, "gen failed");

    std::vector<std::string> days;
    for (const auto& e : fs::directory_iterator(raw))
        if (e.is_directory()) days.push_back(e.path().filename().string());
    std::sort(days.begin(), days.end());
    CHECK(days.size() == 9, "expected 9 day dirs");

    const std::vector<int> ids = {0, 1, 2, 1000, 1001, 1002, 1500, 1501, 1502};
    std::mt19937_64 rng(42);

    for (uint64_t stride : {4, 5, 256}) {
        const fs::path store = tmp / ("gen_store_" + std::to_string(stride));
        CHECK(runIngest(raw, store, stride) == 0, "ingest failed, stride " << stride);
        Store s(store.string());
        CHECK(s.nInstruments() == 1503 && s.stride() == stride, "header");
        CHECK(s.count(3) == 0 && s.count(999) == 0 && s.count(1499) == 0, "missing ids have no data");

        uint64_t total = 0;
        for (int id : ids) {
            std::vector<Record> all;
            for (const std::string& d : days) {
                const auto part = readRecords(fileFor(raw, d, id));
                all.insert(all.end(), part.begin(), part.end());
            }
            total += all.size();
            CHECK(!all.empty() && s.count(id) == all.size(), "count id " << id);
            CHECK(std::memcmp(s.records(id), all.data(), all.size() * sizeof(Record)) == 0,
                  "records differ from concatenated raw, id " << id);

            // Query times: around records (incl. bracket starts), gap midpoints, extremes, random.
            std::vector<int64_t> times = {all.front().timestampNs - 1, all.front().timestampNs,
                                          all.back().timestampNs, all.back().timestampNs + 1, 0,
                                          INT64_MAX};
            for (size_t i = 0; i < all.size(); i += 997) {
                for (int64_t dt : {-1, 0, 1}) times.push_back(all[i].timestampNs + dt);
            }
            for (size_t i = 0; i < all.size(); i += stride * 251) {
                for (int64_t dt : {-1, 0, 1}) times.push_back(all[i].timestampNs + dt);
            }
            for (size_t i = 1; i < all.size(); ++i) {
                const int64_t gap = all[i].timestampNs - all[i - 1].timestampNs;
                if (gap > 600'000'000'000LL) {  // overnight, weekend, holiday, Tokyo lunch
                    times.push_back(all[i - 1].timestampNs + gap / 2);
                    CHECK(s.lastAtOrBefore(id, all[i - 1].timestampNs + gap / 2) == (int64_t)i - 1, "gap last");
                    CHECK(s.firstAtOrAfter(id, all[i - 1].timestampNs + gap / 2) == (int64_t)i, "gap first");
                }
            }
            std::uniform_int_distribution<int64_t> any(all.front().timestampNs, all.back().timestampNs);
            for (int i = 0; i < 200; ++i) times.push_back(any(rng));

            for (int64_t t : times) {
                // Linear scan reference.
                int64_t last = -1, first = -1;
                for (size_t i = 0; i < all.size(); ++i) {
                    if (all[i].timestampNs <= t) last = static_cast<int64_t>(i);
                    if (first < 0 && all[i].timestampNs >= t) first = static_cast<int64_t>(i);
                }
                CHECK(s.lastAtOrBefore(id, t) == last, "last id " << id << " t=" << t << " stride " << stride);
                CHECK(s.firstAtOrAfter(id, t) == first, "first id " << id << " t=" << t << " stride " << stride);
            }
        }
        CHECK(s.totalRecords() == total, "total records");

        // Segments: calendar counts and hand-computed UTC starts (Jan 2 is winter time).
        // 2025-01-02 is day 20090; UTC seconds = day * 86400 + hh:mm.
        const int64_t d0 = 20090 * 86400;
        struct SegCase { int id; size_t count; int64_t firstStart, firstEnd; };
        const SegCase segCases[] = {
            {0, 6, d0 + 14 * 3600 + 1800, d0 + 21 * 3600},          // US 09:30-16:00 EST, 6 days (Jan 9 closed)
            {1000, 7, d0 + 8 * 3600, d0 + 16 * 3600 + 1800},        // Berlin 09:00-17:30 CET, 7 days
            {1500, 14, d0 + 0, d0 + 2 * 3600 + 1800}};              // Tokyo morning 00:00-02:30 UTC, 7 days x 2
        for (const SegCase& c : segCases) {
            const auto segs = s.segments(c.id);
            CHECK(segs.size() == c.count, "segment count id " << c.id << " got " << segs.size());
            if (segs.empty()) continue;
            CHECK(segs[0].startNs == c.firstStart * 1'000'000'000LL && segs[0].endNs == c.firstEnd * 1'000'000'000LL,
                  "first segment bounds id " << c.id);
            const Record* r = s.records(c.id);
            uint64_t prevEnd = 0;
            for (const Segment& g : segs) {
                CHECK(g.begin < g.end && g.begin >= prevEnd, "segment order id " << c.id);
                CHECK(r[g.begin].timestampNs >= g.startNs && r[g.end - 1].timestampNs < g.endNs, "segment holds its records");
                CHECK(g.begin == 0 || r[g.begin - 1].timestampNs < g.startNs, "record before segment is outside it");
                CHECK(g.end == s.count(c.id) || r[g.end].timestampNs >= g.endNs, "record after segment is outside it");
                // A full session is 1s spaced: only dropped seconds (~0.5%) can be missing.
                const int64_t seconds = (g.endNs - g.startNs) / 1'000'000'000LL;
                CHECK(static_cast<int64_t>(g.end - g.begin) > seconds * 97 / 100 && static_cast<int64_t>(g.end - g.begin) <= seconds,
                      "segment size id " << c.id);
                prevEnd = g.end;
            }
            CHECK(prevEnd == s.count(c.id), "segments cover all records id " << c.id);
        }
        // Tokyo lunch 02:30-03:30 UTC is in no segment; morning and afternoon are different segments.
        const auto tokyo = s.segments(1500);
        CHECK(s.segmentContaining(1500, (d0 + 2 * 3600 + 1800 + 60) * 1'000'000'000LL) == nullptr, "lunch gap");
        CHECK(s.segmentContaining(1500, (d0 + 3 * 3600 + 1800) * 1'000'000'000LL) == &tokyo[1], "afternoon start");
        CHECK(s.segmentContaining(1500, (d0 + 2 * 3600 + 1800 - 1) * 1'000'000'000LL) == &tokyo[0], "morning last second");
        CHECK(s.segmentContaining(1500, d0 * 1'000'000'000LL - 1) == nullptr, "before first segment");
        CHECK(s.segmentContaining(3, d0 * 1'000'000'000LL) == nullptr, "instrument without data");
        CHECK(s.regionOf(0) == 0 && s.regionOf(1000) == 1 && s.regionOf(1500) == 2 && s.regionOf(5000) == -1, "regionOf");
        CHECK(s.nRegions() == 3 && s.regionName(0) == "US" && s.regionName(1) == "Europe", "region names");

        // Price extrema over random ranges (incl. within one block and across many).
        for (int id : ids) {
            const Record* r = s.records(id);
            const uint64_t n = s.count(id);
            std::uniform_int_distribution<uint64_t> pick(0, n - 1);
            for (int i = 0; i < 300; ++i) {
                uint64_t a = pick(rng), b = pick(rng);
                if (i % 3 == 0) b = std::min(n - 1, a + pick(rng) % (3 * stride));  // short ranges
                if (a > b) std::swap(a, b);
                float lo, hi, wantLo = r[a].price, wantHi = r[a].price;
                for (uint64_t k = a; k <= b; ++k) {
                    wantLo = std::min(wantLo, r[k].price);
                    wantHi = std::max(wantHi, r[k].price);
                }
                s.priceExtrema(id, a, b + 1, lo, hi);
                CHECK(lo == wantLo && hi == wantHi, "extrema id " << id << " [" << a << "," << b << "] stride " << stride);
            }
            float lo, hi;
            s.priceExtrema(id, 0, n, lo, hi);  // whole instrument, incl. the ragged last block
            float wantLo = r[0].price, wantHi = r[0].price;
            for (uint64_t k = 0; k < n; ++k) { wantLo = std::min(wantLo, r[k].price); wantHi = std::max(wantHi, r[k].price); }
            CHECK(lo == wantLo && hi == wantHi, "whole-range extrema id " << id);
        }
    }
}

// Many adjacent instruments written by many threads: neighbours share pages of data.db
// and index.idx, and concurrent unaligned writes to one page once lost data on macOS
// (zeroed record ranges at the start of ~17% of instruments). Every byte is compared.
static void testManyInstruments(const fs::path& tmp) {
    const fs::path raw = tmp / "many_raw";
    CHECK(runCmd(std::string(GEN_BIN) + " --config " + CONFIG_PATH + " --out " + raw.string() +
                 " --seed 7 --start 2025-01-06 --end 2025-01-08 --limit 60") == 0, "gen failed");
    const fs::path store = tmp / "many_store";
    CHECK(runIngest(raw, store, 256) == 0, "ingest failed");
    Store s(store.string());
    const std::vector<std::string> days = {"20250106", "20250107", "20250108"};
    size_t checked = 0;
    for (int id = 0; id < 2000; ++id) {
        std::vector<Record> all;
        for (const std::string& d : days) {
            const fs::path f = fileFor(raw, d, id);
            if (!fs::exists(f)) continue;
            const auto part = readRecords(f);
            all.insert(all.end(), part.begin(), part.end());
        }
        if (all.empty()) { CHECK(s.count(id) == 0, "count of absent id " << id); continue; }
        ++checked;
        CHECK(s.firstRecord(id) % kDataAlignRecords == 0, "instrument " << id << " must start on its own 64 KiB block");
        CHECK(s.count(id) == all.size(), "count id " << id);
        CHECK(std::memcmp(s.records(id), all.data(), all.size() * sizeof(Record)) == 0, "records differ, id " << id);
        // The index must agree with the data: block k starts at record k * stride.
        for (uint64_t k = 0; k * 256 < all.size(); k += 97)
            CHECK(s.lastAtOrBefore(id, all[k * 256].timestampNs) == static_cast<int64_t>(k * 256), "index entry, id " << id);
        float lo, hi, wantLo = all[0].price, wantHi = all[0].price;
        for (const Record& r : all) { wantLo = std::min(wantLo, r.price); wantHi = std::max(wantHi, r.price); }
        s.priceExtrema(id, 0, all.size(), lo, hi);
        CHECK(lo == wantLo && hi == wantHi, "block ranges, id " << id);
    }
    CHECK(checked == 180, "expected 180 instruments with data, got " << checked);
}

int main() {
    const fs::path tmp = TEST_TMP_DIR;
    fs::remove_all(tmp / "ingest");
    fs::create_directories(tmp / "ingest");
    testHandBuilt(tmp / "ingest");
    testCorruptInput(tmp / "ingest");
    testGenerated(tmp / "ingest");
    testManyInstruments(tmp / "ingest");
    if (failures == 0) std::cout << "all ingest tests passed\n";
    return failures == 0 ? 0 : 1;
}

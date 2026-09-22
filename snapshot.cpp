// sim snapshot: last known price of every instrument at each input timestamp.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>

#include "sim_cmds.h"

namespace sim {

void snapshotRow(const Store& store, int64_t ts, float* row, ThreadPool& pool) {
    const uint64_t n = std::min<uint64_t>(store.nInstruments(), kSnapshotWidth);
    std::fill(row, row + kSnapshotWidth, std::numeric_limits<float>::quiet_NaN());
    // Instruments are independent lookups, each touching ~one page of data.db, so
    // spreading them over many threads overlaps the page faults on a cold cache.
    pool.run(n, 16, [&](size_t begin, size_t end) {
        for (size_t id = begin; id < end; ++id) {
            float price;
            if (store.lastKnownPrice(id, ts, price)) row[id] = price;
        }
    });
}

namespace {

std::vector<int64_t> parseTimestamps(const std::string& text) {
    std::vector<int64_t> ts;
    std::string_view rest = text;
    size_t line = 0;
    while (!rest.empty()) {
        const size_t nl = rest.find('\n');
        std::string_view row = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
        ++line;
        while (!row.empty() && (row.back() == '\r' || row.back() == ' ')) row.remove_suffix(1);
        while (!row.empty() && row.front() == ' ') row.remove_prefix(1);
        if (row.empty()) continue;
        int64_t v = 0;
        const auto res = std::from_chars(row.data(), row.data() + row.size(), v);
        if (res.ec != std::errc() || res.ptr != row.data() + row.size())
            throw std::runtime_error("timestamps line " + std::to_string(line) + ": bad number");
        ts.push_back(v);
    }
    return ts;
}

double percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) return 0;
    const size_t k = std::min(sorted.size() - 1, static_cast<size_t>(std::ceil(p * sorted.size())) - 1);
    return sorted[k];
}

}  // namespace

RunInfo cmdSnapshot(Args& args) {
    const std::string storeDir = args.required("store");
    const std::string inPath = args.required("in");
    const std::string outPath = args.required("out");
    const unsigned threads = static_cast<unsigned>(args.number("threads", defaultThreads()));
    const bool warm = args.number("warm-index", 1) != 0;
    args.check();

    Stopwatch clock;
    const Store store(storeDir);
    const std::vector<int64_t> stamps = parseTimestamps(readFile(inPath));
    if (warm) store.warmIndex();  // index load is startup cost, reported apart from query latency
    const double startup = clock.seconds();

    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) throw std::runtime_error("cannot open " + outPath);
    ThreadPool pool(threads);
    std::vector<float> row(kSnapshotWidth);
    std::vector<double> micros;
    micros.reserve(stamps.size());
    for (int64_t ts : stamps) {
        const auto t0 = std::chrono::steady_clock::now();
        snapshotRow(store, ts, row.data(), pool);
        micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        if (std::fwrite(row.data(), sizeof(float), kSnapshotWidth, out) != kSnapshotWidth)
            throw std::runtime_error("failed to write " + outPath);
    }
    if (std::fclose(out) != 0) throw std::runtime_error("failed to write " + outPath);

    std::sort(micros.begin(), micros.end());
    double sum = 0;
    for (double m : micros) sum += m;
    const double median = percentile(micros, 0.5), p99 = percentile(micros, 0.99);
    const double mean = micros.empty() ? 0 : sum / micros.size();
    std::cerr << stamps.size() << " snapshots; startup " << startup << "s; latency per snapshot: median "
              << median << " us, p99 " << p99 << " us, mean " << mean << " us, max "
              << (micros.empty() ? 0 : micros.back()) << " us (" << pool.size() << " threads)\n";

    RunInfo info;
    info.resultJson = "\"snapshots\":" + std::to_string(stamps.size()) +
                      ",\"startup_seconds\":" + std::to_string(startup) +
                      ",\"median_us\":" + std::to_string(median) + ",\"p99_us\":" + std::to_string(p99);
    return info;
}

}  // namespace sim

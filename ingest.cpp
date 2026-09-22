// Ingest: turns raw/YYYYMMDD/NNNN.bin (grouped by day, then instrument) into a
// store grouped by instrument (see store_format.h).
//
//   ./ingest --raw raw/ --store store/ [--config region.yaml] [--stride N] [--threads N]
//
// Pass 1 stats every raw file, which fixes each instrument's position in
// data.db and index.idx up front. Pass 2 runs instruments in parallel: read an
// instrument's day files in date order into one buffer, check it is time-sorted,
// build the block index (first timestamp and min/max price per stride records) and
// the segment list (calendar sessions from region.yaml), and pwrite the records and
// index to their final positions. Threads write disjoint, page-aligned regions of data.db
// and disjoint slices of in-memory index arrays that one thread writes at the end.
// Output goes to *.tmp files that are renamed once complete.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "store_format.h"

namespace {

using namespace sim;
namespace fs = std::filesystem;

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "records are little-endian on disk");

struct Options {
    std::string raw;
    std::string store;
    std::string configPath = "region.yaml";
    uint64_t stride = kDefaultStride;
    unsigned threads = 0;  // 0 = hardware concurrency
    bool verify = false;   // read every instrument back from data.db and compare
};

void usage(const char* prog) {
    std::cerr << "usage: " << prog << " --raw DIR --store DIR [--config FILE] [--stride N] [--threads N] [--verify]\n";
}

Options parseArgs(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
            return argv[++i];
        };
        if (flag == "--raw") opt.raw = value();
        else if (flag == "--store") opt.store = value();
        else if (flag == "--config") opt.configPath = value();
        else if (flag == "--stride") opt.stride = std::stoull(value());
        else if (flag == "--threads") opt.threads = static_cast<unsigned>(std::stoul(value()));
        else if (flag == "--verify") opt.verify = true;
        else if (flag == "--help" || flag == "-h") { usage(argv[0]); std::exit(0); }
        else throw std::runtime_error("unknown argument " + flag);
    }
    if (opt.raw.empty() || opt.store.empty()) throw std::runtime_error("--raw and --store are required");
    if (opt.stride == 0) throw std::runtime_error("--stride must be positive");
    if (opt.threads == 0) opt.threads = std::max(1u, std::thread::hardware_concurrency());
    return opt;
}

bool allDigits(const std::string& s, size_t len) {
    return s.size() == len && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// RAII file descriptor.
class Fd {
public:
    Fd(const std::string& path, int flags, mode_t mode = 0644) : path_(path) {
        fd_ = ::open(path.c_str(), flags, mode);
        if (fd_ < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
    }
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const { return fd_; }

    void close() {
        if (::close(fd_) != 0) throw std::runtime_error("close failed for " + path_);
        fd_ = -1;
    }

    // pread/pwrite that finish partial transfers and cap each call at 1 GiB.
    void readAt(void* buf, size_t len, uint64_t offset) const {
        char* p = static_cast<char*>(buf);
        while (len > 0) {
            const ssize_t n = ::pread(fd_, p, std::min<size_t>(len, 1u << 30), static_cast<off_t>(offset));
            if (n <= 0) throw std::runtime_error("short read from " + path_);
            p += n; offset += n; len -= n;
        }
    }
    void writeAt(const void* buf, size_t len, uint64_t offset) const {
        const char* p = static_cast<const char*>(buf);
        while (len > 0) {
            const ssize_t n = ::pwrite(fd_, p, std::min<size_t>(len, 1u << 30), static_cast<off_t>(offset));
            if (n <= 0) throw std::runtime_error("write failed for " + path_);
            p += n; offset += n; len -= n;
        }
    }

private:
    std::string path_;
    int fd_ = -1;
};

struct RawLayout {
    std::vector<std::string> dayNames;   // sorted YYYYMMDD, i.e. chronological
    uint64_t nInstruments = 0;           // max id + 1
    std::vector<uint64_t> bytes;         // [day * nInstruments + id]; 0 = empty or absent
};

RawLayout scanRaw(const fs::path& rawDir) {
    if (!fs::is_directory(rawDir)) throw std::runtime_error("not a directory: " + rawDir.string());
    RawLayout layout;
    for (const auto& e : fs::directory_iterator(rawDir))
        if (e.is_directory() && allDigits(e.path().filename().string(), 8))
            layout.dayNames.push_back(e.path().filename().string());
    std::sort(layout.dayNames.begin(), layout.dayNames.end());

    struct Found { size_t day; uint64_t id; uint64_t bytes; };
    std::vector<Found> found;
    for (size_t d = 0; d < layout.dayNames.size(); ++d) {
        for (const auto& e : fs::directory_iterator(rawDir / layout.dayNames[d])) {
            const fs::path& p = e.path();
            if (p.extension() != ".bin" || !allDigits(p.stem().string(), 4)) continue;
            const uint64_t size = e.file_size();
            if (size % sizeof(Record) != 0)
                throw std::runtime_error(p.string() + ": size is not a multiple of 16 bytes");
            const uint64_t id = std::stoull(p.stem().string());
            found.push_back({d, id, size});
            layout.nInstruments = std::max(layout.nInstruments, id + 1);
        }
    }
    layout.bytes.assign(layout.dayNames.size() * layout.nInstruments, 0);
    for (const Found& f : found) layout.bytes[f.day * layout.nInstruments + f.id] = f.bytes;
    return layout;
}

int run(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    const fs::path rawDir = opt.raw;
    const RawLayout layout = scanRaw(rawDir);
    const uint64_t nDays = layout.dayNames.size();
    const uint64_t n = layout.nInstruments;
    std::cerr << "raw: " << nDays << " days, " << n << " instruments, scanned in " << elapsed() << "s\n";

    // Final position of every instrument, from pass 1.
    std::vector<InstrumentEntry> table(n);
    uint64_t totalRecords = 0, totalEntries = 0, cursor = 0;
    for (uint64_t id = 0; id < n; ++id) {
        uint64_t bytes = 0;
        for (uint64_t d = 0; d < nDays; ++d) bytes += layout.bytes[d * n + id];
        const uint64_t count = bytes / sizeof(Record);
        if (count > 0) cursor = alignUp(cursor);  // own pages: see store_format.h
        table[id] = {cursor, count, totalEntries};
        cursor += count;
        totalRecords += count;
        totalEntries += numOfIndexEntries(count, opt.stride);
    }
    const uint64_t dataRecords = alignUp(cursor);
    const uint64_t indexBase = sizeof(StoreHeader) + n * sizeof(InstrumentEntry);
    const uint64_t rangeBase = indexBase + totalEntries * sizeof(int64_t);
    std::vector<int64_t> allSparse(totalEntries);      // filled by workers (disjoint slices),
    std::vector<BlockRange> allRanges(totalEntries);   // written to index.idx once, below

    // Calendar: which region each id belongs to, and the local trading dates present.
    const Config cfg = loadConfig(opt.configPath);
    if (cfg.regions.size() > kMaxRegions) throw std::runtime_error("too many regions in config");
    std::vector<int64_t> days;
    for (const std::string& name : layout.dayNames)
        days.push_back(daysFromCivil(std::stoi(name.substr(0, 4)), std::stoi(name.substr(4, 2)),
                                     std::stoi(name.substr(6, 2))));
    auto regionIndexOf = [&](uint64_t id) -> int {
        for (size_t r = 0; r < cfg.regions.size(); ++r) {
            const Region& reg = cfg.regions[r];
            if (id >= static_cast<uint64_t>(reg.firstSecurityId) &&
                id < static_cast<uint64_t>(reg.firstSecurityId + reg.nStocks))
                return static_cast<int>(r);
        }
        return -1;
    };
    std::vector<std::vector<Segment>> segments(n);  // each slot written by one worker

    const fs::path storeDir = opt.store;
    fs::create_directories(storeDir);
    const std::string dataTmp = (storeDir / (std::string(kDataFile) + ".tmp")).string();
    const std::string indexTmp = (storeDir / (std::string(kIndexFile) + ".tmp")).string();
    Fd dataFile(dataTmp, O_RDWR | O_CREAT | O_TRUNC);
    Fd indexFile(indexTmp, O_RDWR | O_CREAT | O_TRUNC);
    if (::ftruncate(dataFile.get(), static_cast<off_t>(dataRecords * sizeof(Record))) != 0 ||
        ::ftruncate(indexFile.get(), static_cast<off_t>(indexFileSize(n, totalEntries))) != 0)
        throw std::runtime_error("cannot size output files (disk full?)");

    StoreHeader header{};
    std::memcpy(header.magic, kStoreMagic, sizeof header.magic);
    header.version = kStoreVersion;
    header.recordSize = sizeof(Record);
    header.stride = opt.stride;
    header.nInstruments = n;
    header.totalRecords = totalRecords;
    header.totalRecordsWithPadding = dataRecords;
    header.totalIndexEntries = totalEntries;

    std::atomic<uint64_t> nextId{0}, done{0};
    std::atomic<bool> failed{false};
    std::exception_ptr firstError;
    std::mutex errorMutex;

    auto worker = [&] {
        std::unique_ptr<Record[]> buf;  // uninitialised, grown to the largest instrument seen
        uint64_t capacity = 0;
        std::unique_ptr<Record[]> check;  // read-back buffer for --verify
        uint64_t checkCapacity = 0;
        char name[16];
        try {
            for (uint64_t id; !failed && (id = nextId.fetch_add(1)) < n;) {
                const InstrumentEntry& e = table[id];
                if (e.count > capacity) {
                    buf.reset(new Record[e.count]);
                    capacity = e.count;
                }
                std::snprintf(name, sizeof name, "%04llu.bin", static_cast<unsigned long long>(id));
                uint64_t filled = 0;
                for (uint64_t d = 0; d < nDays; ++d) {
                    const uint64_t bytes = layout.bytes[d * n + id];
                    if (bytes == 0) continue;
                    const std::string path = (rawDir / layout.dayNames[d] / name).string();
                    Fd f(path, O_RDONLY);
                    f.readAt(buf.get() + filled, bytes, 0);
                    filled += bytes / sizeof(Record);
                }
                if (filled != e.count)
                    throw std::runtime_error("raw files changed during ingest (instrument " +
                                             std::to_string(id) + ")");
                for (uint64_t i = 1; i < e.count; ++i)
                    if (buf[i].timestampNs <= buf[i - 1].timestampNs)
                        throw std::runtime_error("instrument " + std::to_string(id) +
                                                 ": timestamps not strictly increasing at record " +
                                                 std::to_string(i));

                int64_t* sparse = allSparse.data() + e.firstIndexPos;
                BlockRange* ranges = allRanges.data() + e.firstIndexPos;
                for (uint64_t i = 0; i < e.count; i += opt.stride) {
                    *sparse++ = buf[i].timestampNs;
                    const uint64_t stop = std::min(i + opt.stride, e.count);
                    BlockRange r{buf[i].price, buf[i].price};
                    for (uint64_t j = i + 1; j < stop; ++j) {
                        r.minPrice = std::min(r.minPrice, buf[j].price);
                        r.maxPrice = std::max(r.maxPrice, buf[j].price);
                    }
                    *ranges++ = r;
                }

                // Segments: each calendar session on each local trading date present in raw.
                const int region = regionIndexOf(id);
                if (region >= 0 && e.count > 0) {
                    const Region& reg = cfg.regions[region];
                    auto firstAtOrAfter = [&](int64_t ts) {
                        return static_cast<uint64_t>(
                            std::lower_bound(buf.get(), buf.get() + e.count, ts,
                                             [](const Record& rec, int64_t v) { return rec.timestampNs < v; }) -
                            buf.get());
                    };
                    for (int64_t day : days) {
                        if (!isTradingDay(reg, day)) continue;
                        for (size_t k = 0; k < reg.sessions.size(); ++k) {
                            const int64_t startNs =
                                local2utc(reg, day * kSecondsPerDay + reg.sessions[k].startSec) * 1'000'000'000LL;
                            const int64_t endNs =
                                local2utc(reg, day * kSecondsPerDay + reg.sessions[k].endSec) * 1'000'000'000LL;
                            const uint64_t begin = firstAtOrAfter(startNs), end = firstAtOrAfter(endNs);
                            if (begin < end)
                                segments[id].push_back({startNs, endNs, begin, end,
                                                        static_cast<int32_t>(day), static_cast<int32_t>(k)});
                        }
                    }
                }

                dataFile.writeAt(buf.get(), e.count * sizeof(Record), e.firstRecordPos * sizeof(Record));
                if (opt.verify && e.count > 0) {
                    if (e.count > checkCapacity) {
                        check.reset(new Record[e.count]);
                        checkCapacity = e.count;
                    }
                    dataFile.readAt(check.get(), e.count * sizeof(Record), e.firstRecordPos * sizeof(Record));
                    if (std::memcmp(check.get(), buf.get(), e.count * sizeof(Record)) != 0)
                        throw std::runtime_error("verify failed: instrument " + std::to_string(id) +
                                                 " differs from what was written");
                }

                const uint64_t finished = ++done;
                if (finished % 250 == 0 || finished == n) {
                    char line[64];
                    std::snprintf(line, sizeof line, "%llu/%llu instruments, %.1fs\n",
                                  static_cast<unsigned long long>(finished),
                                  static_cast<unsigned long long>(n), elapsed());
                    std::fputs(line, stderr);
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (!firstError) firstError = std::current_exception();
            failed = true;
        }
    };

    std::vector<std::thread> pool;
    for (unsigned i = 0; i < std::min<uint64_t>(opt.threads, std::max<uint64_t>(n, 1)); ++i)
        pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    if (firstError) std::rethrow_exception(firstError);

    dataFile.close();

    // Index: one thread, one pass, then read back and compare (it is small).
    indexFile.writeAt(&header, sizeof header, 0);
    indexFile.writeAt(table.data(), n * sizeof(InstrumentEntry), sizeof header);
    indexFile.writeAt(allSparse.data(), allSparse.size() * sizeof(int64_t), indexBase);
    indexFile.writeAt(allRanges.data(), allRanges.size() * sizeof(BlockRange), rangeBase);
    {
        std::vector<char> back(indexFileSize(n, totalEntries));
        indexFile.readAt(back.data(), back.size(), 0);
        const bool same =
            std::memcmp(back.data(), &header, sizeof header) == 0 &&
            std::memcmp(back.data() + sizeof header, table.data(), n * sizeof(InstrumentEntry)) == 0 &&
            std::memcmp(back.data() + indexBase, allSparse.data(), allSparse.size() * sizeof(int64_t)) == 0 &&
            std::memcmp(back.data() + rangeBase, allRanges.data(), allRanges.size() * sizeof(BlockRange)) == 0;
        if (!same) throw std::runtime_error("verify failed: index.idx differs from what was written");
    }
    indexFile.close();

    // Segment file: header (with region names), per-instrument table, then all segments.
    SegmentsHeader segHeader{};
    std::memcpy(segHeader.magic, kSegmentsMagic, sizeof segHeader.magic);
    segHeader.version = 1;
    segHeader.nRegions = static_cast<uint32_t>(cfg.regions.size());
    segHeader.nInstruments = n;
    for (size_t r = 0; r < cfg.regions.size(); ++r)
        std::strncpy(segHeader.regionNames[r], cfg.regions[r].name.c_str(), 15);
    std::vector<InstrumentSegment> segTable(n);
    std::vector<Segment> allSegments;
    for (uint64_t id = 0; id < n; ++id) {
        const int region = regionIndexOf(id);
        segTable[id] = {allSegments.size(), static_cast<uint32_t>(segments[id].size()),
                        region < 0 ? kNoRegion : static_cast<uint32_t>(region)};
        allSegments.insert(allSegments.end(), segments[id].begin(), segments[id].end());
    }
    segHeader.totalSegments = allSegments.size();
    const std::string segTmp = (storeDir / (std::string(kSegmentsFile) + ".tmp")).string();
    {
        Fd segFile(segTmp, O_RDWR | O_CREAT | O_TRUNC);
        segFile.writeAt(&segHeader, sizeof segHeader, 0);
        segFile.writeAt(segTable.data(), n * sizeof(InstrumentSegment), sizeof segHeader);
        segFile.writeAt(allSegments.data(), allSegments.size() * sizeof(Segment),
                        sizeof segHeader + n * sizeof(InstrumentSegment));
        segFile.close();
    }

    // Index last: a store only exists once index.idx does.
    fs::rename(dataTmp, storeDir / kDataFile);
    fs::rename(segTmp, storeDir / kSegmentsFile);
    fs::rename(indexTmp, storeDir / kIndexFile);

    const uint64_t storeBytes = dataRecords * sizeof(Record) + indexFileSize(n, totalEntries) +
                                segmentsFileSize(n, allSegments.size());
    std::cerr << "ingested " << totalRecords << " points in " << elapsed() << "s; store "
              << storeBytes << " bytes";
    if (totalRecords > 0)
        std::cerr << " (" << static_cast<double>(storeBytes) / static_cast<double>(totalRecords)
                  << " bytes/point)";
    std::cerr << '\n';
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

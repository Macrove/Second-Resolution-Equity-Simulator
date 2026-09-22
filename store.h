// Read-only view of a store written by ./ingest (layout in store_format.h).
//
// Timestamp lookups find the index block for the timestamp (binary search over
// the instrument's block timestamps), then binary search inside that block of at
// most `stride` records. Results are record positions within the instrument, or
// -1 when there is no such record. data.db is memory-mapped for random access;
// readRecords() streams from the same file with pread for big sequential scans
// (keeps resident memory bounded, unlike touching a 46 GB mapping).
#pragma once

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "store_format.h"

namespace sim {

class Store {
public:
    explicit Store(const std::string& dir) {
        //"out/index.idx"
        indexMap_ = mapFile(dir + "/" + kIndexFile, indexBytes_);
        if (indexBytes_ < sizeof(StoreHeader)) throw std::runtime_error("index file too small");
        std::memcpy(&header_, indexMap_, sizeof header_);
        if (std::memcmp(header_.magic, kStoreMagic, sizeof kStoreMagic) != 0 ||
            header_.version != kStoreVersion || header_.recordSize != sizeof(Record) ||
            header_.stride == 0)
            throw std::runtime_error("not a store, or unsupported version: " + dir);
        if (indexBytes_ != indexFileSize(header_.nInstruments, header_.totalIndexEntries))
            throw std::runtime_error("index file size does not match its header");

        table_ = reinterpret_cast<const InstrumentEntry*>(indexMap_ + sizeof(StoreHeader));
        sparse_ = reinterpret_cast<const int64_t*>(table_ + header_.nInstruments);
        ranges_ = reinterpret_cast<const BlockRange*>(sparse_ + header_.totalIndexEntries);

        // dir/data.db
        dataMap_ = mapFile(dir + "/" + kDataFile, dataBytes_, &dataFd_);
        if (dataBytes_ != header_.totalRecordsWithPadding * sizeof(Record))
            throw std::runtime_error("data file size does not match the index header");
        data_ = reinterpret_cast<const Record*>(dataMap_);

        // dir/segments.idx
        segMap_ = mapFile(dir + "/" + kSegmentsFile, segBytes_);
        if (segBytes_ < sizeof(SegmentsHeader)) throw std::runtime_error("segments file too small");
        std::memcpy(&segHeader_, segMap_, sizeof segHeader_);
        if (std::memcmp(segHeader_.magic, kSegmentsMagic, sizeof kSegmentsMagic) != 0 ||
            segHeader_.nInstruments != header_.nInstruments ||
            segBytes_ != segmentsFileSize(segHeader_.nInstruments, segHeader_.totalSegments))
            throw std::runtime_error("segments file does not match the store");
        segTable_ = reinterpret_cast<const InstrumentSegment*>(segMap_ + sizeof(SegmentsHeader));
        segments_ = reinterpret_cast<const Segment*>(segTable_ + segHeader_.nInstruments);
    }

    ~Store() {
        if (indexMap_) munmap(const_cast<char*>(indexMap_), indexBytes_);
        if (dataMap_) munmap(const_cast<char*>(dataMap_), dataBytes_);
        if (segMap_) munmap(const_cast<char*>(segMap_), segBytes_);
        if (dataFd_ >= 0) ::close(dataFd_);
    }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    uint64_t nInstruments() const { return header_.nInstruments; }
    uint64_t stride() const { return header_.stride; }
    uint64_t totalRecords() const { return header_.totalRecords; }

    // Position of the instrument's first record in data.db, in records.
    uint64_t firstRecord(uint64_t id) const { return id < header_.nInstruments ? table_[id].firstRecordPos : 0; }

    uint64_t count(uint64_t id) const { return id < header_.nInstruments ? table_[id].count : 0; }

    // The instrument's records, time-sorted; count(id) of them.
    const Record* records(uint64_t id) const {
        return id < header_.nInstruments ? data_ + table_[id].firstRecordPos : nullptr;
    }

    // Records [begin, end) of the instrument, through the mapping.
    std::span<const Record> range(uint64_t id, uint64_t begin, uint64_t end) const {
        return {records(id) + begin, static_cast<size_t>(end - begin)};
    }

    // Copies records [begin, begin + n) of the instrument into dst with pread.
    void readRecords(uint64_t id, uint64_t begin, uint64_t n, Record* dst) const {
        char* p = reinterpret_cast<char*>(dst);
        uint64_t len = n * sizeof(Record);
        uint64_t off = (table_[id].firstRecordPos + begin) * sizeof(Record);
        while (len > 0) {
            const ssize_t got = ::pread(dataFd_, p, std::min<uint64_t>(len, 1u << 30), static_cast<off_t>(off));
            if (got <= 0) throw std::runtime_error("read from data file failed");
            p += got; off += got; len -= got;
        }
    }

    // Last record with timestamp <= t ("last known price"); -1 if t is before the first.
    int64_t lastAtOrBefore(uint64_t id, int64_t t) const {
        const uint64_t n = count(id);
        if (n == 0) return -1;
        const int64_t* idx = sparse_ + table_[id].firstIndexPos;
        const uint64_t nEntries = numOfIndexEntries(n, header_.stride);
        const uint64_t after = std::upper_bound(idx, idx + nEntries, t) - idx;
        if (after == 0) return -1;
        // Record (after-1)*stride has timestamp <= t, so the block always holds an answer.
        const Record* r = records(id);
        const uint64_t lo = (after - 1) * header_.stride;
        const uint64_t hi = std::min(lo + header_.stride, n);
        const Record* p = std::upper_bound(r + lo, r + hi, t, [](int64_t v, const Record& rec) {
            return v < rec.timestampNs;
        });
        return (p - r) - 1;
    }

    // First record with timestamp >= t (entry snap); -1 if t is after the last.
    int64_t firstAtOrAfter(uint64_t id, int64_t t) const {
        const uint64_t n = count(id);
        if (n == 0) return -1;
        const int64_t* idx = sparse_ + table_[id].firstIndexPos;
        const uint64_t nEntries = numOfIndexEntries(n, header_.stride);
        // Blocks starting before `first` begin at timestamps < t, so the answer lies in the
        // last of them or is record first * stride itself (timestamp >= t).
        const uint64_t first = std::lower_bound(idx, idx + nEntries, t) - idx;
        if (first == 0) return 0;
        const Record* r = records(id);
        const uint64_t lo = (first - 1) * header_.stride;
        const uint64_t hi = std::min(first * header_.stride, n);
        const Record* p = std::lower_bound(r + lo, r + hi, t, [](const Record& rec, int64_t v) {
            return rec.timestampNs < v;
        });
        const uint64_t pos = p - r;
        return pos == n ? -1 : static_cast<int64_t>(pos);
    }

    // Price of the last record at or before t; false if there is none.
    bool lastKnownPrice(uint64_t id, int64_t t, float& price) const {
        const int64_t pos = lastAtOrBefore(id, t);
        if (pos < 0) return false;
        price = records(id)[pos].price;
        return true;
    }

    // Min and max price over records [begin, end) (begin < end). Whole blocks come from
    // the index; only the ragged ends are read from data.db.
    void priceExtrema(uint64_t id, uint64_t begin, uint64_t end, float& lo, float& hi) const {
        const Record* r = records(id);
        const uint64_t s = header_.stride;
        const uint64_t headEnd = (begin + s - 1) / s * s;  // first block boundary >= begin
        const uint64_t tailStart = end / s * s;            // last block boundary <= end
        lo = r[begin].price;
        hi = lo;
        auto scan = [&](uint64_t a, uint64_t b) {
            for (uint64_t i = a; i < b; ++i) {
                lo = std::min(lo, r[i].price);
                hi = std::max(hi, r[i].price);
            }
        };
        if (headEnd >= tailStart) {
            scan(begin, end);
            return;
        }
        scan(begin, headEnd);
        const BlockRange* blocks = ranges_ + table_[id].firstIndexPos;
        for (uint64_t k = headEnd / s; k < tailStart / s; ++k) {
            lo = std::min(lo, blocks[k].minPrice);
            hi = std::max(hi, blocks[k].maxPrice);
        }
        scan(tailStart, end);
    }

    // Segments (calendar sessions that have data) of the instrument, in time order.
    std::span<const Segment> segments(uint64_t id) const {
        if (id >= segHeader_.nInstruments) return {};
        return {segments_ + segTable_[id].first, segTable_[id].count};
    }

    // The segment whose calendar session contains t; nullptr in a gap (night, weekend,
    // holiday, Tokyo lunch) or if the session has no data. Not called by any sim command
    // yet (only test_ingest.cpp exercises it) — it's here for strategy code that needs
    // "is id trading right now", as opposed to segments(), which strategy.cpp already
    // uses to walk every session.
    const Segment* segmentContaining(uint64_t id, int64_t t) const {
        const auto segs = segments(id);
        auto it = std::upper_bound(segs.begin(), segs.end(), t,
                                   [](int64_t v, const Segment& s) { return v < s.startNs; });
        if (it == segs.begin()) return nullptr;
        --it;
        return t < it->endNs ? &*it : nullptr;
    }

    // Region index of the instrument, or -1.
    int regionOf(uint64_t id) const {
        if (id >= segHeader_.nInstruments || segTable_[id].regionId == kNoRegion) return -1;
        return static_cast<int>(segTable_[id].regionId);
    }
    uint32_t nRegions() const { return segHeader_.nRegions; }
    std::string regionName(uint32_t r) const {
        return std::string(segHeader_.regionNames[r], strnlen(segHeader_.regionNames[r], 16));
    }

    // Reads the whole index once so later lookups do not page-fault on it.
    uint64_t warmIndex() const {
        madvise(const_cast<char*>(indexMap_), indexBytes_, MADV_WILLNEED);
        uint64_t sum = 0;
        for (uint64_t i = 0; i < indexBytes_; i += 4096) sum += static_cast<uint8_t>(indexMap_[i]);
        return sum;
    }

private:
    // Maps a whole file read-only; an empty file maps to nullptr. Keeps the fd if asked.
    static const char* mapFile(const std::string& path, uint64_t& bytes, int* keepFd = nullptr) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path);
        struct stat st;
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("cannot stat " + path);
        }
        bytes = static_cast<uint64_t>(st.st_size);
        void* p = nullptr;
        if (bytes > 0) {
            p = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                throw std::runtime_error("cannot mmap " + path);
            }
        }
        if (keepFd) *keepFd = fd;
        else ::close(fd);
        return static_cast<const char*>(p);
    }

    const char* indexMap_ = nullptr;
    const char* dataMap_ = nullptr;
    const char* segMap_ = nullptr;
    uint64_t indexBytes_ = 0, dataBytes_ = 0, segBytes_ = 0;
    int dataFd_ = -1;
    StoreHeader header_{};
    SegmentsHeader segHeader_{};
    const InstrumentEntry* table_ = nullptr;
    const int64_t* sparse_ = nullptr;
    const BlockRange* ranges_ = nullptr;
    const Record* data_ = nullptr;
    const InstrumentSegment* segTable_ = nullptr;
    const Segment* segments_ = nullptr;
};

}  // namespace sim

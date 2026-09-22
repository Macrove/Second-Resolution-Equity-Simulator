// On-disk layout of the store written by ./ingest and read by store.h.
//
//   store/data.db      Every instrument's records, in id order, each instrument
//                      time-sorted. Pure 16-byte records, no header. Each instrument
//                      starts on a 64 KiB boundary: the gap before it is zero-filled
//                      padding (bytes are literally 0, from ftruncate), not "no padding".
//                      This is so ingest threads never write into the same page: concurrent
//                      unaligned writes to a shared page lost data on macOS/APFS. 64 KiB is
//                      used instead of the actual page size (16 KiB on Apple Silicon, 4 KiB
//                      elsewhere) so one alignment is safely larger than any page size we'll
//                      hit, without retuning per platform.
//   store/index.idx    StoreHeader, then InstrumentEntry[nInstruments] (indexed by
//                      security id, so lookup is arithmetic), then for every
//                      instrument ceil(count / stride) blocks of `stride` records:
//                        int64      timestamp of the block's first record   (all blocks)
//                        BlockRange min/max price inside the block          (all blocks)
//                      stored as two parallel arrays. Timestamps are their own array
//                      so the binary search over them stays dense.
//   store/segments.idx SegmentsHeader, InstrumentSegment[nInstruments], Segment[].
//                      Calendar facts (session hours, DST, holidays), never inferred
//                      from gaps in the data, because dropped seconds look like gaps.
//                      Lets Store answer "is id in a live session at t" and "which
//                      sessions/days does id have data for" without redoing calendar
//                      math at query time; strategy.cpp is the main reader.
#pragma once

#include "sim_common.h"

namespace sim {

constexpr char kStoreMagic[8] = {'S', 'R', 'E', 'S', 'T', 'O', 'R', '2'};
constexpr char kSegmentsMagic[8] = {'S', 'R', 'S', 'E', 'G', 'M', 'T', '1'};
constexpr uint32_t kStoreVersion = 2;
constexpr uint64_t kDefaultStride = 256;  // records per index block (4 KB)
// instrument start alignment in data.db: 64 KiB, in records (see comment at the top of this file).
constexpr uint64_t kDataAlignRecords = 65536 / sizeof(Record);

// rounds up to next align records
constexpr uint64_t alignUp(uint64_t records) {
    return (records + kDataAlignRecords - 1) / kDataAlignRecords * kDataAlignRecords;
}

constexpr const char* kDataFile = "data.db";
constexpr const char* kIndexFile = "index.idx";
constexpr const char* kSegmentsFile = "segments.idx";

struct StoreHeader {
    char magic[8];
    uint32_t version;
    uint32_t recordSize;
    uint64_t stride;
    uint64_t nInstruments;       // max security id + 1; ids without data have count 0
    uint64_t totalRecords;       // sum of all instruments' counts
    uint64_t totalRecordsWithPadding;  // length of data.db in records, padding included.
    // Sum over all instruments of numOfIndexEntries(count, stride): how many blocks the
    // sparse/ranges arrays below hold in total, i.e. their size and where each instrument's
    // slice of them ends.
    uint64_t totalIndexEntries;
};
static_assert(sizeof(StoreHeader) == 56);

struct InstrumentEntry {
    uint64_t firstRecordPos;  // position of this instrument's first record in data.db, in records
    uint64_t count;
    uint64_t firstIndexPos;   // position of its first block in the index arrays, in blocks.
};
static_assert(sizeof(InstrumentEntry) == 24);

// Min/max price over one index block's records, so Store::priceExtrema() can answer whole
// blocks straight from the index and only has to scan data.db for the ragged partial
// blocks at the two ends of a range.
struct BlockRange {
    float minPrice;
    float maxPrice;
};
static_assert(sizeof(BlockRange) == 8);

constexpr uint64_t numOfIndexEntries(uint64_t count, uint64_t stride) {
    return (count + stride - 1) / stride;
}

constexpr uint64_t indexFileSize(uint64_t nInstruments, uint64_t totalIndexEntries) {
    return sizeof(StoreHeader) + nInstruments * sizeof(InstrumentEntry) +
           totalIndexEntries * (sizeof(int64_t) + sizeof(BlockRange));
}

constexpr uint32_t kMaxRegions = 8;
constexpr uint32_t kNoRegion = UINT32_MAX;

struct SegmentsHeader {
    char magic[8];
    uint32_t version;
    uint32_t nRegions;
    uint64_t nInstruments;
    uint64_t totalSegments;
    char regionNames[kMaxRegions][16];
};
static_assert(sizeof(SegmentsHeader) == 160);

struct InstrumentSegment {
    uint64_t first;     // position of its first Segment
    uint32_t count;
    uint32_t regionId;  // kNoRegion if the id belongs to no configured region
};
static_assert(sizeof(InstrumentSegment) == 16);

// One continuous block of trading that actually has records. [startNs, endNs) is
// the calendar session in UTC.
struct Segment {
    int64_t startNs;
    int64_t endNs;
    uint64_t begin;   // first record position (within the instrument) at or after startNs
    uint64_t end;     // first record position (within the instrument) at or after endNs
    int32_t day;      // local trading date, days since 1970-01-01
    int32_t session;  // index of the session within the day (Tokyo: 0 morning, 1 afternoon)
};
static_assert(sizeof(Segment) == 40);

constexpr uint64_t segmentsFileSize(uint64_t nInstruments, uint64_t totalSegments) {
    return sizeof(SegmentsHeader) + nInstruments * sizeof(InstrumentSegment) +
           totalSegments * sizeof(Segment);
}

}  // namespace sim

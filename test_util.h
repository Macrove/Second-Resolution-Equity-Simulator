// Helpers shared by the test programs: CHECK, raw-file writers, running commands.
#pragma once

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "store_format.h"

namespace fs = std::filesystem;
using namespace sim;

inline int failures = 0;
#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++failures;                                                              \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond " -- " << msg << '\n'; \
        }                                                                            \
    } while (0)

inline std::vector<Record> readRecords(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    std::vector<Record> r(bytes.size() / sizeof(Record));
    std::memcpy(r.data(), bytes.data(), r.size() * sizeof(Record));
    return r;
}

inline void writeBytes(const fs::path& p, const void* data, size_t len) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
}

inline void writeRecords(const fs::path& p, const std::vector<Record>& r) {
    writeBytes(p, r.data(), r.size() * sizeof(Record));
}

inline fs::path fileFor(const fs::path& raw, const std::string& day, int id) {
    char name[16];
    std::snprintf(name, sizeof name, "%04d.bin", id);
    return raw / day / name;
}

inline int runCmd(const std::string& cmd) { return std::system((cmd + " 2>/dev/null").c_str()); }


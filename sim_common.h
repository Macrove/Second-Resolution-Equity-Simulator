// Small helpers shared by the sim subcommands: argument parsing, a reusable
// thread pool, number formatting, peak memory and the run log.
#pragma once

#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sim {

// One second-resolution tick: the unit record of the raw files, data.db and
// every buffer built from them. Shared here (rather than in store_format.h,
// which is only the store's on-disk layout) because generator.cpp writes it
// and code with no reason to know the store's layout still needs it.
struct Record {
    int64_t timestampNs;
    float price;
    int32_t volume;
};
static_assert(sizeof(Record) == 16, "record must be 16 bytes");

// "--key value" pairs after the subcommand. get() marks a key used; check()
// rejects anything left over so typos do not silently fall back to defaults.
class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 0; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag.rfind("--", 0) != 0) throw std::runtime_error("unexpected argument " + flag);
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
            values_[flag.substr(2)] = argv[++i];
        }
    }
    std::string str(const std::string& key, const std::string& fallback = "") {
        auto it = values_.find(key);
        if (it == values_.end()) return fallback;
        used_[key] = true;
        return it->second;
    }
    std::string required(const std::string& key) {
        const std::string v = str(key);
        if (v.empty()) throw std::runtime_error("--" + key + " is required");
        return v;
    }
    double number(const std::string& key) { return std::stod(required(key)); }
    double number(const std::string& key, double fallback) {
        return values_.count(key) ? std::stod(str(key)) : fallback;
    }
    void check() const {
        for (const auto& kv : values_)
            if (!used_.count(kv.first)) throw std::runtime_error("unknown option --" + kv.first);
    }
    const std::map<std::string, std::string>& all() const { return values_; }

private:
    std::map<std::string, std::string> values_;
    std::map<std::string, bool> used_;
};

// Threads are created once; run() splits [0, total) into chunks that the workers
// and the calling thread pull dynamically. Blocks until every chunk is done.
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads) {
        for (unsigned i = 1; i < std::max(1u, threads); ++i)
            workers_.emplace_back([this] { workerLoop(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cvStart_.notify_all();
        for (std::thread& t : workers_) t.join();
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned size() const { return static_cast<unsigned>(workers_.size()) + 1; }

    void run(size_t total, size_t chunk, const std::function<void(size_t, size_t)>& fn) {
        if (total == 0) return;
        {
            std::lock_guard<std::mutex> lock(m_);
            fn_ = &fn;
            total_ = total;
            chunk_ = std::max<size_t>(1, chunk);
            next_ = 0;
            pending_ = workers_.size();
            ++generation_;
        }
        cvStart_.notify_all();
        drain();
        std::unique_lock<std::mutex> lock(m_);
        cvDone_.wait(lock, [this] { return pending_ == 0; });
    }

private:
    void drain() {
        for (;;) {
            const size_t begin = next_.fetch_add(chunk_);
            if (begin >= total_) return;
            (*fn_)(begin, std::min(begin + chunk_, total_));
        }
    }
    void workerLoop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_);
                cvStart_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            drain();
            std::lock_guard<std::mutex> lock(m_);
            if (--pending_ == 0) cvDone_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cvStart_, cvDone_;
    const std::function<void(size_t, size_t)>* fn_ = nullptr;
    size_t total_ = 0, chunk_ = 1;
    std::atomic<size_t> next_{0};
    size_t pending_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

inline unsigned defaultThreads() { return std::max(1u, std::thread::hardware_concurrency()); }

// Shortest text that round-trips.
inline void appendNumber(std::string& out, double v) {
    char buf[40];
    out.append(buf, std::to_chars(buf, buf + sizeof buf, v).ptr);
}
inline void appendNumber(std::string& out, float v) {
    char buf[40];
    out.append(buf, std::to_chars(buf, buf + sizeof buf, v).ptr);
}
inline void appendNumber(std::string& out, int64_t v) {
    char buf[24];
    out.append(buf, std::to_chars(buf, buf + sizeof buf, v).ptr);
}

inline uint64_t peakRssBytes() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return static_cast<uint64_t>(ru.ru_maxrss);  // bytes
#else
    return static_cast<uint64_t>(ru.ru_maxrss) * 1024;  // kilobytes
#endif
}

class Stopwatch {
public:
    double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }
private:
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

//TODO: this reads data char by char, can be slow. optimize this
inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline void writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f) throw std::runtime_error("failed to write " + path);
}

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out;
}

inline std::string isoTimeUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// One JSON object per line; a single append write so concurrent runs cannot interleave.
inline void appendRunLog(const std::string& path, const std::string& jsonLine) {
    std::ofstream f(path, std::ios::app | std::ios::binary);
    const std::string line = jsonLine + "\n";
    f.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace sim

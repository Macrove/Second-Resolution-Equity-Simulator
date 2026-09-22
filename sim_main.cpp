// ./sim trades|snapshot|strategy ... : see README for the options of each.
//
// Every run is timed, its peak resident memory read, and one JSON line appended to
// the run log (default runs.jsonl, "--runlog none" to skip) for the dashboard.
#include <iostream>

#include "sim_cmds.h"

namespace {

using namespace sim;

void usage(const char* prog) {
    std::cerr << "usage:\n"
              << "  " << prog << " trades   --store DIR --in trades.csv --out results.csv [--threads N] [--mae blocks|scan]\n"
              << "  " << prog << " snapshot --store DIR --in timestamps.txt --out snapshots.bin [--threads N] [--warm-index 0|1]\n"
              << "  " << prog << " strategy --store DIR --reverters FILE --window W --entry E --exit X --max-hold H\n"
              << "        --out trades.csv --summary summary.csv [--threads N]\n"
              << "  all commands: [--runlog FILE|none]\n";
}

std::string paramsJson(const Args& args) {
    std::string js = "{";
    for (const auto& kv : args.all()) {
        if (kv.first == "runlog") continue;
        if (js.size() > 1) js += ",";
        js += "\"" + jsonEscape(kv.first) + "\":\"" + jsonEscape(kv.second) + "\"";
    }
    return js + "}";
}

int run(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        usage(argv[0]);
        return 0;
    }
    Args args(argc - 2, argv + 2);
    const std::string runLog = args.str("runlog", "runs.jsonl");
    const std::string params = paramsJson(args);

    Stopwatch clock;
    RunInfo info;
    if (command == "trades") info = cmdTrades(args);
    else if (command == "snapshot") info = cmdSnapshot(args);
    else if (command == "strategy") info = cmdStrategy(args);
    else throw std::runtime_error("unknown command " + command);

    const double wall = clock.seconds();
    const uint64_t peak = peakRssBytes();
    std::cerr << command << ": wall " << wall << "s, peak memory " << peak / (1024.0 * 1024.0) << " MiB\n";

    if (runLog != "none") {
        std::string line = "{\"time\":\"" + isoTimeUtc() + "\",\"command\":\"" + command + "\",\"params\":" + params +
                           ",\"wall_seconds\":" + std::to_string(wall) + ",\"peak_rss_bytes\":" + std::to_string(peak);
        if (!info.resultJson.empty()) line += "," + info.resultJson;
        appendRunLog(runLog, line + "}");
    }
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

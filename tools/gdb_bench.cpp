// gdb_bench - measure what a long-lived memory store actually costs.
//
//   gdb_bench [nodes] [edges-per-node]
//
// Answers the questions that decide whether this design survives ten years:
// how much RAM per memory, how long a save takes, how long opening the file
// takes, and where the per-node bytes actually go.

#include "gdb/gdb.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

namespace {

std::size_t currentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
        return static_cast<std::size_t>(info.WorkingSetSize);
    }
#endif
    return 0;
}

std::size_t peakBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
        return static_cast<std::size_t>(info.PeakWorkingSetSize);
    }
#endif
    return 0;
}

std::string megabytes(std::size_t bytes) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// A plausible memory record: a text blob, a kind, a timestamp and a source.
std::string filler(std::size_t i) {
    std::string text = "session memory entry #" + std::to_string(i) +
                       " -- the user asked about embedding a graph store into a C++ service";
    text.resize(80, '.');
    return text;
}

struct Measurement {
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t withPropsBytes = 0;
    std::size_t bareBytes = 0;
    double buildSeconds = 0.0;
    double saveSeconds = 0.0;
    double loadSeconds = 0.0;
    double queryMicros = 0.0;
    double hopMicros = 0.0;
    double scanMicros = 0.0;
    std::size_t estimated = 0;
    std::size_t fileBytes = 0;
};

Measurement run(std::size_t nodeCount, std::size_t edgesPerNode, bool withProps) {
    Measurement m;
    m.nodes = nodeCount;

    const std::size_t before = currentBytes();

    gdb::Database db;
    std::vector<gdb::Id> ids;
    ids.reserve(nodeCount);

    const auto buildStart = Clock::now();
    const char* kinds[4] = {"fact", "concept", "episode", "skill"};
    for (std::size_t i = 0; i < nodeCount; ++i) {
        if (withProps) {
            ids.push_back(db.addNode("Memory",
                                     {{"text", gdb::Value::text(filler(i))},
                                      {"kind", gdb::Value::text(kinds[i % 4])},
                                      {"ts", gdb::Value::text(std::to_string(1700000000 + i))},
                                      {"src", gdb::Value::text("session-" + std::to_string(i % 500))}}));
        } else {
            ids.push_back(db.addNode("Memory", {}));
        }
    }

    const std::size_t edgeTotal = nodeCount * edgesPerNode;
    for (std::size_t i = 0; i < edgeTotal; ++i) {
        const gdb::Id from = ids[i % nodeCount];
        // A cheap scramble so edges are not all local neighbours.
        const gdb::Id to = ids[(i * 7919u + 13u) % nodeCount];
        if (from != to) db.addEdge("RELATES", from, to, {});
    }
    m.edges = db.edgeCount();
    m.buildSeconds = secondsSince(buildStart);
    m.withPropsBytes = currentBytes() - before;
    m.estimated = db.estimatedBytes();

    std::filesystem::path path = std::filesystem::temp_directory_path() / "gdb_bench.gdb";
    std::filesystem::remove(path);
    const auto saveStart = Clock::now();
    db.save(path.string());
    m.saveSeconds = secondsSince(saveStart);
    m.fileBytes = static_cast<std::size_t>(std::filesystem::file_size(path));

    const auto loadStart = Clock::now();
    gdb::Database reloaded = gdb::Database::open(path.string());
    m.loadSeconds = secondsSince(loadStart);

    // Point lookup through the property index.
    const std::size_t probes = 1000;
    const auto queryStart = Clock::now();
    std::size_t hits = 0;
    for (std::size_t i = 0; i < probes; ++i) {
        const std::size_t target = (i * 7919u) % nodeCount;
        auto r = reloaded.query("MATCH (m:Memory {ts: $t}) RETURN m.text",
                                {{"t", gdb::Value::text(std::to_string(1700000000 + target))}});
        hits += r.size();
    }
    m.queryMicros = secondsSince(queryStart) * 1e6 / static_cast<double>(probes);

    // One hop out of a node.
    const auto hopStart = Clock::now();
    std::size_t reached = 0;
    for (std::size_t i = 0; i < probes; ++i) {
        auto r = reloaded.query("MATCH (:Memory {ts: $t})-[:RELATES]->(n) RETURN n.text",
                                {{"t", gdb::Value::text(std::to_string(1700000000 + ((i * 31u) % nodeCount)))}});
        reached += r.size();
    }
    m.hopMicros = secondsSince(hopStart) * 1e6 / static_cast<double>(probes);

    // Fuzzy lookup: the exact spelling is unknown, so the label is scanned and
    // every row is materialised before WHERE filters it. This is the shape that
    // does not scale, and it is worth a number rather than an assumption.
    if (withProps) {
        const std::size_t scanProbes = 10;
        const auto scanStart = Clock::now();
        for (std::size_t i = 0; i < scanProbes; ++i) {
            auto r = reloaded.query(
                "MATCH (m:Memory) WHERE m.text CONTAINS 'entry #42' RETURN m.text LIMIT 10");
            hits += r.size();
        }
        m.scanMicros = secondsSince(scanStart) * 1e6 / static_cast<double>(scanProbes);
    }

    if (withProps && hits == 0 && reached == 0) {
        std::printf("  warning: probes returned nothing, the timings are meaningless\n");
    }
    std::filesystem::remove(path);
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t nodeCount = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    const std::size_t edgesPerNode = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2;

    std::printf("gdb_bench  %zu nodes, %zu edges per node\n\n", nodeCount, edgesPerNode);

    std::printf("measuring a bare graph (nodes and edges only)...\n");
    const Measurement bare = run(nodeCount, edgesPerNode, false);

    std::printf("measuring a realistic graph (4 properties per node)...\n");
    const Measurement full = run(nodeCount, edgesPerNode, true);

    const double perNodeFull = static_cast<double>(full.withPropsBytes) / static_cast<double>(nodeCount);
    const double perNodeBare = static_cast<double>(bare.withPropsBytes) / static_cast<double>(nodeCount);
    const double perProp = (perNodeFull - perNodeBare) / 4.0;

    std::printf("\n--- one node's cost ---\n");
    std::printf("  nodes + edges only        %8.0f bytes per node\n", perNodeBare);
    std::printf("  plus 4 properties         %8.0f bytes per node\n", perNodeFull);
    std::printf("  of which each property    %8.0f bytes  (record + string + index)\n", perProp);

    std::printf("\n--- %zu nodes / %zu edges ---\n", full.nodes, full.edges);
    std::printf("  resident memory           %s\n", megabytes(full.withPropsBytes).c_str());
    std::printf("  self-estimated            %s   (%.0f%% of measured, this is what the "
                "capacity warning reports)\n",
                megabytes(full.estimated).c_str(),
                100.0 * static_cast<double>(full.estimated) /
                    static_cast<double>(full.withPropsBytes));
    std::printf("  peak working set          %s\n", megabytes(peakBytes()).c_str());
    std::printf("  build time                %.2f s\n", full.buildSeconds);
    std::printf("  .gdb file size            %s\n", megabytes(full.fileBytes).c_str());
    std::printf("  save (whole file rewrite) %.2f s\n", full.saveSeconds);
    std::printf("  open (full load)          %.3f s\n", full.loadSeconds);
    std::printf("  point lookup by property  %.1f us\n", full.queryMicros);
    std::printf("  one hop traversal         %.1f us\n", full.hopMicros);
    std::printf("  label scan + CONTAINS     %.1f us   <-- does not scale\n", full.scanMicros);

    std::printf("\n--- extrapolated ---\n");
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    for (double perDay : {100.0, 1000.0, 10000.0}) {
        const double nodesPerYear = 365.0 * perDay;
        const double gbPerYear = perNodeFull * nodesPerYear / kGiB;
        std::printf("  %8.0f memories/day -> %10.0f nodes/year, %7.3f GB/year, %7.2f GB after 10 years\n",
                    perDay, nodesPerYear, gbPerYear, gbPerYear * 10);
    }
    return 0;
}

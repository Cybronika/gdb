// gdb_cli - command line access to a .gdb file.
//
//   gdb_cli query  <db> "<cypher>" [--json]      一条查询
//   gdb_cli exec   <db> <script.cypher> [--dry-run] [--json]   执行脚本（导入）
//   gdb_cli export <db> [--json]                 导出为 Cypher 脚本或 JSON
//   gdb_cli info   <db> [--json]                 规模、标签、关系类型
//   gdb_cli <db> [ "<cypher>" ]                  REPL，或一条查询
//
// On Windows this uses wmain and converts to UTF-8 itself. The C runtime would
// otherwise turn the command line into char** through the ANSI code page, which
// mangles any non-ASCII query before the library ever sees it.

#include "gdb/gdb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// platform
// ---------------------------------------------------------------------------

#ifdef _WIN32
std::string toUtf8(const wchar_t* wide) {
    if (!wide) return {};
    // size includes the terminating NUL, so the buffer has to be that long.
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1));
    return out;
}

// The console defaults to the legacy code page, which mangles UTF-8 output.
void useUtf8Console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
void useUtf8Console() {}
#endif

// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------

void jsonString(std::ostream& out, const std::string& text) {
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out << buffer;
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    out << '"';
}

void writeJsonList(std::ostream& out, const std::vector<std::string>& items) {
    out << '[';
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out << ',';
        jsonString(out, items[i]);
    }
    out << ']';
}

void jsonProperties(std::ostream& out, const gdb::Properties& props) {
    out << '{';
    bool first = true;
    for (const gdb::Prop& p : props) {
        if (!first) out << ',';
        first = false;
        jsonString(out, p.key);
        out << ':';
        // Property values are text or a list of text.
        if (p.value.type == gdb::ValueType::Null) out << "null";
        else if (p.value.type == gdb::ValueType::List) writeJsonList(out, p.value.items);
        else jsonString(out, p.value.s);
    }
    out << '}';
}

// Scalars become plain JSON. Graph references become objects tagged with
// "@node" / "@rel", so a consumer can tell a node from a string that looks
// like one.
void jsonValue(std::ostream& out, const gdb::Database& db, const gdb::Value& v) {
    switch (v.type) {
    case gdb::ValueType::Null: out << "null"; break;
    case gdb::ValueType::Str: jsonString(out, v.s); break;
    case gdb::ValueType::List: writeJsonList(out, v.items); break;
    case gdb::ValueType::Node:
        out << "{\"@node\":" << v.ref << ",\"label\":";
        jsonString(out, db.nodeLabel(v.ref));
        out << ",\"properties\":";
        jsonProperties(out, db.nodeProps(v.ref));
        out << '}';
        break;
    case gdb::ValueType::Rel:
        out << "{\"@rel\":" << v.ref << ",\"type\":";
        jsonString(out, db.edgeType(v.ref));
        out << ",\"from\":" << db.edgeSource(v.ref) << ",\"to\":" << db.edgeTarget(v.ref)
            << ",\"properties\":";
        jsonProperties(out, db.edgeProps(v.ref));
        out << '}';
        break;
    }
}

void jsonStats(std::ostream& out, const gdb::QueryResult& r) {
    out << "{\"nodes_created\":" << r.nodes_created << ",\"nodes_deleted\":" << r.nodes_deleted
        << ",\"edges_created\":" << r.edges_created << ",\"edges_deleted\":" << r.edges_deleted
        << ",\"properties_set\":" << r.props_set << '}';
}

void printJsonResult(const gdb::Database& db, const gdb::QueryResult& r) {
    std::ostringstream out;
    out << "{\"columns\":[";
    for (std::size_t i = 0; i < r.columns.size(); ++i) {
        if (i) out << ',';
        jsonString(out, r.columns[i]);
    }
    out << "],\"rows\":[";
    for (std::size_t row = 0; row < r.rows.size(); ++row) {
        if (row) out << ',';
        out << '[';
        for (std::size_t c = 0; c < r.columns.size(); ++c) {
            if (c) out << ',';
            jsonValue(out, db, r.at(row, c));
        }
        out << ']';
    }
    out << "],\"stats\":";
    jsonStats(out, r);
    out << "}\n";
    std::cout << out.str();
}

// ---------------------------------------------------------------------------
// Cypher output
// ---------------------------------------------------------------------------

std::string quoteText(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out += "'";
    return out;
}

std::string cypherLiteral(const gdb::Value& v) {
    switch (v.type) {
    case gdb::ValueType::Null: return "null";
    case gdb::ValueType::List: {
        std::string out = "[";
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            if (i) out += ", ";
            out += quoteText(v.items[i]);
        }
        out += "]";
        return out;
    }
    default: return quoteText(v.s);
    }
}

std::string cypherProperties(const gdb::Properties& props) {
    if (props.empty()) return {};
    std::string out = " {";
    bool first = true;
    for (const gdb::Prop& p : props) {
        if (!first) out += ", ";
        first = false;
        out += p.key;
        out += ": ";
        out += cypherLiteral(p.value);
    }
    out += "}";
    return out;
}

// One CREATE clause holding every node and then every relationship. Nodes are
// listed first so the relationship patterns can refer to their variables, which
// is exactly how this would be written by hand -- and how it re-imports.
void exportCypher(const gdb::Database& db, std::ostream& out) {
    const std::vector<gdb::Id> nodes = db.allNodes();
    const std::vector<gdb::Id> edges = db.allEdges();

    std::map<gdb::Id, std::string> names;
    for (std::size_t i = 0; i < nodes.size(); ++i) names[nodes[i]] = "n" + std::to_string(i);

    out << "// gdb export: " << nodes.size() << " nodes, " << edges.size() << " relationships\n";
    if (nodes.empty() && edges.empty()) {
        out << "// nothing to export\n";
        return;
    }

    out << "CREATE ";
    bool first = true;
    for (gdb::Id id : nodes) {
        if (!first) out << ",\n       ";
        first = false;
        out << '(' << names[id];
        const std::string label = db.nodeLabel(id);
        if (!label.empty()) out << ':' << label;
        out << cypherProperties(db.nodeProps(id));
        out << ')';
    }
    for (gdb::Id id : edges) {
        if (!first) out << ",\n       ";
        first = false;
        out << '(' << names[db.edgeSource(id)] << ")-[:" << db.edgeType(id)
            << cypherProperties(db.edgeProps(id)) << "]->(" << names[db.edgeTarget(id)] << ')';
    }
    out << ";\n";
}

// ---------------------------------------------------------------------------
// script splitting
// ---------------------------------------------------------------------------

// Splits on ';' while ignoring semicolons inside strings, backtick identifiers
// and comments, so a script can be executed statement by statement.
std::vector<std::string> splitStatements(const std::string& text) {
    std::vector<std::string> statements;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;
    bool inBacktick = false;
    bool inLineComment = false;
    bool inBlockComment = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';

        if (inLineComment) {
            current.push_back(c);
            if (c == '\n') inLineComment = false;
            continue;
        }
        if (inBlockComment) {
            current.push_back(c);
            if (c == '*' && next == '/') {
                current.push_back(next);
                ++i;
                inBlockComment = false;
            }
            continue;
        }
        if (inSingle || inDouble) {
            current.push_back(c);
            if (c == '\\' && next != '\0') {
                current.push_back(next);
                ++i;
            } else if ((inSingle && c == '\'') || (inDouble && c == '"')) {
                inSingle = inDouble = false;
            }
            continue;
        }
        if (inBacktick) {
            current.push_back(c);
            if (c == '`') inBacktick = false;
            continue;
        }

        if (c == '/' && next == '/') {
            current.push_back(c);
            current.push_back(next);
            ++i;
            inLineComment = true;
            continue;
        }
        if (c == '/' && next == '*') {
            current.push_back(c);
            current.push_back(next);
            ++i;
            inBlockComment = true;
            continue;
        }
        if (c == '\'') { inSingle = true; current.push_back(c); continue; }
        if (c == '"') { inDouble = true; current.push_back(c); continue; }
        if (c == '`') { inBacktick = true; current.push_back(c); continue; }

        if (c == ';') {
            statements.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    statements.push_back(current);

    std::vector<std::string> out;
    for (std::string& s : statements) {
        if (s.find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(std::move(s));
    }
    return out;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw gdb::Error("cannot open '" + path + "' for reading");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Windows editors like to prefix a UTF-8 BOM. The lexer would treat those
    // three bytes as identifier characters, so drop them here.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

struct Options {
    bool json = false;
    bool force = false;
    bool dryRun = false;
    bool fresh = false;
};

// Personal memory accumulates on the order of ten nodes a night: about 3,650 a
// year, 36,500 in a decade. Ten times that leaves generous headroom for a much
// busier pipeline, so a graph beyond it almost certainly means world knowledge
// was imported in bulk -- which is redundant with the model's weights and
// degrades retrieval rather than adding to it.
constexpr std::size_t kPlausibleMemoryNodes = 500000;

std::string megabytes(std::size_t bytes) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

void printStats(const gdb::Database& db) {
    std::printf("nodes: %zu   edges: %zu   tombstones: %zu   generation: %llu   ~%s\n",
                db.nodeCount(), db.edgeCount(), db.tombstoneCount(),
                static_cast<unsigned long long>(db.generation()),
                megabytes(db.estimatedBytes()).c_str());
    if (db.tombstoneCount() > (db.nodeCount() + db.edgeCount()) / 4) {
        std::printf("  note: more than a quarter of the records are deleted leftovers; "
                    "run 'gdb_cli compact <db>' to rebuild the file cleanly\n");
    }
}

void collectVocabulary(const gdb::Database& db, std::set<std::string>& labels,
                       std::set<std::string>& types) {
    for (gdb::Id id : db.allNodes()) labels.insert(db.nodeLabel(id));
    for (gdb::Id id : db.allEdges()) types.insert(db.edgeType(id));
}

int commandQuery(const Options& opt, const std::string& path, const std::string& cypher) {
    gdb::Database db = gdb::Database::open(path);
    gdb::QueryResult r = db.query(cypher);
    if (opt.json) {
        printJsonResult(db, r);
    } else if (!r.columns.empty()) {
        std::cout << r.dump(db);
    }
    const bool wrote = r.nodes_created || r.nodes_deleted || r.edges_created || r.edges_deleted ||
                       r.props_set;
    if (wrote) {
        db.save(path, opt.force);
    }
    return 0;
}

int commandExec(const Options& opt, const std::string& path, const std::string& script) {
    const std::vector<std::string> statements = splitStatements(readFile(script));
    if (statements.empty()) {
        std::fprintf(stderr, "error: '%s' contains no statements\n", script.c_str());
        return 1;
    }

    gdb::Database db = gdb::Database::open(path);

    std::set<std::string> labelsBefore;
    std::set<std::string> typesBefore;
    collectVocabulary(db, labelsBefore, typesBefore);

    gdb::QueryResult total;
    std::vector<std::string> failures;
    for (std::size_t i = 0; i < statements.size(); ++i) {
        try {
            gdb::QueryResult r = db.query(statements[i]);
            total.nodes_created += r.nodes_created;
            total.nodes_deleted += r.nodes_deleted;
            total.edges_created += r.edges_created;
            total.edges_deleted += r.edges_deleted;
            total.props_set += r.props_set;
        } catch (const std::exception& e) {
            // Report the failing statement by index and line so it can be found.
            std::size_t line = 1;
            for (std::size_t k = 0; k < i; ++k) {
                for (char c : statements[k]) {
                    if (c == '\n') ++line;
                }
            }
            failures.push_back("statement " + std::to_string(i + 1) + ": " + e.what());
            break;
        }
    }

    // Vocabulary drift is the quiet killer: a nightly extractor that invents a
    // near-synonym for an existing relationship type makes later traversal miss
    // rows with no error at all. Surfacing new names here is what lets the
    // confirmation step catch it before it is committed.
    std::set<std::string> labelsAfter;
    std::set<std::string> typesAfter;
    collectVocabulary(db, labelsAfter, typesAfter);
    std::vector<std::string> newLabels;
    std::vector<std::string> newTypes;
    for (const std::string& name : labelsAfter) {
        if (!labelsBefore.count(name)) newLabels.push_back(name);
    }
    for (const std::string& name : typesAfter) {
        if (!typesBefore.count(name)) newTypes.push_back(name);
    }

    const bool wrote = total.nodes_created || total.nodes_deleted || total.edges_created ||
                       total.edges_deleted || total.props_set;

    if (opt.json) {
        std::ostringstream out;
        out << "{\"statements\":" << statements.size()
            << ",\"applied\":" << (failures.empty() ? 1 : 0) << ",\"dry_run\":"
            << (opt.dryRun ? 1 : 0) << ",\"stats\":";
        jsonStats(out, total);
        out << ",\"new_labels\":[";
        for (std::size_t i = 0; i < newLabels.size(); ++i) {
            if (i) out << ',';
            jsonString(out, newLabels[i]);
        }
        out << "],\"new_relationship_types\":[";
        for (std::size_t i = 0; i < newTypes.size(); ++i) {
            if (i) out << ',';
            jsonString(out, newTypes[i]);
        }
        out << "],\"errors\":[";
        for (std::size_t i = 0; i < failures.size(); ++i) {
            if (i) out << ',';
            jsonString(out, failures[i]);
        }
        out << "]}\n";
        std::cout << out.str();
    } else {
        std::printf("%s: %zu statements, %lld nodes and %lld relationships created, "
                    "%lld nodes and %lld relationships deleted, %lld properties set%s\n",
                    opt.dryRun ? "dry run" : "applied", statements.size(),
                    static_cast<long long>(total.nodes_created),
                    static_cast<long long>(total.edges_created),
                    static_cast<long long>(total.nodes_deleted),
                    static_cast<long long>(total.edges_deleted),
                    static_cast<long long>(total.props_set),
                    opt.dryRun ? " (nothing was written)" : "");
        for (const std::string& name : newLabels) {
            std::printf("  new label: %s\n", name.c_str());
        }
        for (const std::string& name : newTypes) {
            std::printf("  new relationship type: %s\n", name.c_str());
        }
        for (const std::string& f : failures) std::fprintf(stderr, "error: %s\n", f.c_str());
    }

    if (!failures.empty()) return 1;
    if (!opt.dryRun && wrote) db.save(path, opt.force);
    return 0;
}

int commandExport(const Options& opt, const std::string& path) {
    gdb::Database db = gdb::Database::open(path);
    if (!opt.json) {
        if (db.nodeCount() > 200000) {
            std::fprintf(stderr,
                         "warning: exporting %zu nodes as one Cypher statement will be large; "
                         "consider --json for a machine format\n",
                         db.nodeCount());
        }
        exportCypher(db, std::cout);
        return 0;
    }

    std::ostringstream out;
    out << "{\"generation\":" << db.generation() << ",\"nodes\":[";
    const std::vector<gdb::Id> nodes = db.allNodes();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i) out << ',';
        const gdb::Id id = nodes[i];
        out << "{\"id\":" << id << ",\"label\":";
        jsonString(out, db.nodeLabel(id));
        out << ",\"properties\":";
        jsonProperties(out, db.nodeProps(id));
        out << '}';
    }
    out << "],\"edges\":[";
    const std::vector<gdb::Id> edges = db.allEdges();
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (i) out << ',';
        const gdb::Id id = edges[i];
        out << "{\"id\":" << id << ",\"type\":";
        jsonString(out, db.edgeType(id));
        out << ",\"from\":" << db.edgeSource(id) << ",\"to\":" << db.edgeTarget(id)
            << ",\"properties\":";
        jsonProperties(out, db.edgeProps(id));
        out << '}';
    }
    out << "]}\n";
    std::cout << out.str();
    return 0;
}

// Rebuilding through the public API is the only compaction on offer, and
// deliberately so: it lives in the tool, where nothing holds node ids across the
// call. In the library it would be the one operation able to renumber ids under
// a caller and silently redirect a later write to the wrong node.
int commandCompact(const Options& opt, const std::string& path) {
    gdb::Database src = gdb::Database::open(path);
    const std::size_t nodes = src.nodeCount();
    const std::size_t edges = src.edgeCount();
    const std::size_t trash = src.tombstoneCount();

    std::map<gdb::Id, gdb::Id> remap;
    gdb::Database rebuilt;
    for (gdb::Id id : src.allNodes()) {
        remap[id] = rebuilt.addNode(src.nodeLabel(id), src.nodeProps(id));
    }
    for (gdb::Id id : src.allEdges()) {
        rebuilt.addEdge(src.edgeType(id), remap[src.edgeSource(id)], remap[src.edgeTarget(id)],
                        src.edgeProps(id));
    }

    // The rebuild produced a fresh database, so its generation is zero and the
    // save would skip the usual lost-update guard. Check by hand that nothing
    // else wrote to the file while the copy was being made.
    if (gdb::Database::open(path).generation() != src.generation()) {
        std::fprintf(stderr, "error: refusing to compact '%s': another writer changed it\n",
                     path.c_str());
        return 1;
    }
    rebuilt.save(path, true);

    if (opt.json) {
        std::ostringstream out;
        out << "{\"nodes\":" << nodes << ",\"edges\":" << edges << ",\"reclaimed\":" << trash
            << "}\n";
        std::cout << out.str();
    } else {
        std::printf("compacted %s: %zu nodes, %zu edges, %zu deleted leftovers reclaimed\n",
                    path.c_str(), nodes, edges, trash);
        std::printf("  note: node and edge ids changed; this was a rebuild\n");
    }
    return 0;
}

int commandInfo(const Options& opt, const std::string& path) {    gdb::Database db = gdb::Database::open(path);

    std::map<std::string, std::size_t> labels;
    std::map<std::string, std::size_t> types;
    std::set<std::string> keys;
    for (gdb::Id id : db.allNodes()) {
        labels[db.nodeLabel(id)]++;
        for (const gdb::Prop& p : db.nodeProps(id)) keys.insert(p.key);
    }
    for (gdb::Id id : db.allEdges()) types[db.edgeType(id)]++;

    if (opt.json) {
        std::ostringstream out;
        out << "{\"nodes\":" << db.nodeCount() << ",\"edges\":" << db.edgeCount()
            << ",\"tombstones\":" << db.tombstoneCount() << ",\"generation\":" << db.generation()
            << ",\"estimated_bytes\":" << db.estimatedBytes() << ",\"over_capacity\":"
            << (db.nodeCount() > kPlausibleMemoryNodes ? 1 : 0) << ",\"labels\":{";
        bool first = true;
        for (const auto& kv : labels) {
            if (!first) out << ',';
            first = false;
            jsonString(out, kv.first);
            out << ':' << kv.second;
        }
        out << "},\"relationship_types\":{";
        first = true;
        for (const auto& kv : types) {
            if (!first) out << ',';
            first = false;
            jsonString(out, kv.first);
            out << ':' << kv.second;
        }
        out << "},\"property_keys\":[";
        first = true;
        for (const std::string& k : keys) {
            if (!first) out << ',';
            first = false;
            jsonString(out, k);
        }
        out << "]}\n";
        std::cout << out.str();
        return 0;
    }

    std::printf("nodes: %zu   edges: %zu   tombstones: %zu   generation: %llu   ~%s\n",
                db.nodeCount(), db.edgeCount(), db.tombstoneCount(),
                static_cast<unsigned long long>(db.generation()),
                megabytes(db.estimatedBytes()).c_str());
    std::printf("labels:");
    if (labels.empty()) std::printf(" (none)");
    for (const auto& kv : labels) std::printf(" %s(%zu)", kv.first.c_str(), kv.second);
    std::printf("\nrelationship types:");
    if (types.empty()) std::printf(" (none)");
    for (const auto& kv : types) std::printf(" %s(%zu)", kv.first.c_str(), kv.second);
    std::printf("\nproperty keys:");
    if (keys.empty()) std::printf(" (none)");
    for (const std::string& k : keys) std::printf(" %s", k.c_str());
    std::printf("\n");

    if (db.nodeCount() > kPlausibleMemoryNodes) {
        std::fprintf(stderr,
                     "warning: %zu nodes is far beyond what personal memory accumulates "
                     "(about 10 a night is 36,500 in a decade). If it grew this large, world "
                     "knowledge was probably imported in bulk -- that is redundant with the "
                     "model's weights and it degrades retrieval rather than adding to it.\n",
                     db.nodeCount());
    }
    if (types.size() > 200) {
        std::fprintf(stderr,
                     "warning: %zu distinct relationship types. A fragmented vocabulary makes "
                     "traversal miss rows silently: MATCH (a)-[:朋友]->(b) will not find edges "
                     "typed 好友. Feed the existing types into the extraction prompt.\n",
                     types.size());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// REPL
// ---------------------------------------------------------------------------

const char* kHelp =
    "commands:\n"
    "  .help            this text\n"
    "  .stats           node / edge counts\n"
    "  .schema          labels, relationship types and property keys\n"
    "  .save            write the database to disk now\n"
    "  .exit            save and quit\n"
    "A statement runs on its own, or after a trailing ';'.\n";

void printSchema(const gdb::Database& db) {
    std::vector<std::string> labels;
    std::vector<std::string> types;
    std::vector<std::string> keys;
    for (gdb::Id id : db.allNodes()) {
        const std::string label = db.nodeLabel(id);
        if (std::find(labels.begin(), labels.end(), label) == labels.end()) labels.push_back(label);
        for (const gdb::Prop& p : db.nodeProps(id)) {
            if (std::find(keys.begin(), keys.end(), p.key) == keys.end()) keys.push_back(p.key);
        }
    }
    for (gdb::Id id : db.allEdges()) {
        const std::string type = db.edgeType(id);
        if (std::find(types.begin(), types.end(), type) == types.end()) types.push_back(type);
    }
    auto show = [](const char* title, const std::vector<std::string>& values) {
        std::printf("%s:", title);
        if (values.empty()) std::printf(" (none)");
        for (const std::string& v : values) std::printf(" %s", v.empty() ? "<none>" : v.c_str());
        std::printf("\n");
    };
    show("labels", labels);
    show("relationship types", types);
    show("property keys", keys);
}

bool runOne(gdb::Database& db, const std::string& statement, bool json, bool* modified) {
    if (statement.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    try {
        gdb::QueryResult result = db.query(statement);
        if (json) {
            printJsonResult(db, result);
        } else if (!result.columns.empty()) {
            std::cout << result.dump(db);
        }
        const bool wrote = result.nodes_created || result.nodes_deleted || result.edges_created ||
                           result.edges_deleted || result.props_set;
        if (modified) *modified = wrote;
        if (!json) {
            if (wrote) {
                std::printf("%lld nodes and %lld relationships created, %lld nodes and %lld "
                            "relationships deleted, %lld properties set\n",
                            static_cast<long long>(result.nodes_created),
                            static_cast<long long>(result.edges_created),
                            static_cast<long long>(result.nodes_deleted),
                            static_cast<long long>(result.edges_deleted),
                            static_cast<long long>(result.props_set));
            } else if (result.columns.empty()) {
                std::printf("ok\n");
            }
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return false;
    }
}

int repl(const Options& opt, const std::string& path, const std::string& inlineQuery) {
    gdb::Database db;
    try {
        if (!opt.fresh) db.load(path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    if (!inlineQuery.empty()) {
        bool modified = false;
        const bool ok = runOne(db, inlineQuery, opt.json, &modified);
        // A read-only query must not bump the file generation: that would
        // invalidate any other process holding this database open.
        if (modified) {
            try {
                db.save(path, opt.force);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "error: %s\n", e.what());
                return 1;
            }
        }
        return ok ? 0 : 1;
    }

    if (!opt.json) {
        std::printf("gdb  %s  (%zu nodes, %zu edges)   .help for commands\n", path.c_str(),
                    db.nodeCount(), db.edgeCount());
    }

    bool dirty = false;
    std::string pending;
    std::string line;
    while (true) {
        std::printf(pending.empty() ? "gdb> " : "...> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;

        const std::size_t first = line.find_first_not_of(" \t\r\n");
        const std::string trimmed = (first == std::string::npos) ? std::string() : line.substr(first);

        if (pending.empty() && !trimmed.empty() && trimmed[0] == '.') {
            std::istringstream words(trimmed);
            std::string command;
            words >> command;
            if (command == ".help") std::printf("%s", kHelp);
            else if (command == ".stats") printStats(db);
            else if (command == ".schema") printSchema(db);
            else if (command == ".save") {
                try {
                    db.save(path, opt.force);
                    std::printf("saved %s\n", path.c_str());
                    dirty = false;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "error: %s\n", e.what());
                }
            } else if (command == ".exit" || command == ".quit") {
                break;
            } else {
                std::fprintf(stderr, "unknown command %s\n", command.c_str());
            }
            continue;
        }

        pending += line;
        pending.push_back('\n');
        if (trimmed.empty() || trimmed.find(';') != std::string::npos ||
            !std::cin.rdbuf()->in_avail()) {
            bool modified = false;
            if (runOne(db, pending, opt.json, &modified) && modified) dirty = true;
            pending.clear();
        }
    }

    if (dirty) {
        try {
            db.save(path, opt.force);
            std::printf("saved %s\n", path.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }
    return 0;
}

const char* kUsage =
    "usage:\n"
    "  gdb_cli query   <db> \"<cypher>\" [--json]        run one statement\n"
    "  gdb_cli exec    <db> <script.cypher> [--dry-run] [--json]\n"
    "                                                    import a Cypher script\n"
    "  gdb_cli export  <db> [--json]                     dump as a Cypher script\n"
    "  gdb_cli compact <db> [--json]                     rebuild the file, reclaiming\n"
    "                                                    deleted leftovers (ids change)\n"
    "  gdb_cli info    <db> [--json]                     size, labels, types\n"
    "  gdb_cli <db> [ \"<cypher>\" ]                      interactive shell\n"
    "\n"
    "options:\n"
    "  --json    machine readable output\n"
    "  --dry-run execute but write nothing\n"
    "  --force   overwrite even if another writer changed the file\n"
    "  --new     start from an empty database (needs --force)\n";

int gdbMain(const std::vector<std::string>& argv) {
    bool json = false;
    bool force = false;
    bool dryRun = false;
    bool fresh = false;
    std::vector<std::string> args;
    for (const std::string& a : argv) {
        if (a == "--json") json = true;
        else if (a == "--force") force = true;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--new") fresh = true;
        else if (a == "-h" || a == "--help") { std::printf("%s", kUsage); return 0; }
        else args.push_back(a);
    }
    if (args.empty()) {
        std::printf("%s", kUsage);
        return 1;
    }

    const Options opt{json, force, dryRun, fresh};
    const std::string first = args[0];

    if (first == "query" || first == "exec" || first == "export" || first == "compact" ||
        first == "info") {
        args.erase(args.begin());
        if (args.empty()) {
            std::fprintf(stderr, "error: %s needs a database file\n", first.c_str());
            return 1;
        }
        const std::string path = args[0];
        try {
            if (first == "query") {
                if (args.size() < 2) {
                    std::fprintf(stderr, "error: query needs a Cypher statement\n");
                    return 1;
                }
                return commandQuery(opt, path, args[1]);
            }
            if (first == "exec") {
                if (args.size() < 2) {
                    std::fprintf(stderr, "error: exec needs a script file\n");
                    return 1;
                }
                return commandExec(opt, path, args[1]);
            }
            if (first == "export") return commandExport(opt, path);
            if (first == "compact") return commandCompact(opt, path);
            return commandInfo(opt, path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }

    if (fresh && !force) {
        std::fprintf(stderr,
                     "error: --new discards the existing file, add --force if that is what you "
                     "meant\n");
        return 1;
    }
    return repl(opt, args[0], args.size() > 1 ? args[1] : std::string());
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    useUtf8Console();
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) args.push_back(toUtf8(wargv[i]));
    return gdbMain(args);
}
#else
int main(int argc, char** argv) {
    useUtf8Console();
    return gdbMain(std::vector<std::string>(argv + 1, argv + argc));
}
#endif

// gdb.h - a small embedded property-graph database for local AI memory.
//
// Design contract:
//   * header-only public surface, zero third-party dependencies, C++20
//   * no daemon, no server, no threads; the caller owns the process
//   * the whole graph lives in RAM; a .gdb file is a plain snapshot
//   * a deliberately small Cypher subset (see docs/DESIGN.md)
//
// Everything here is noexcept-or-throw. Nothing returns a silently wrong
// answer: malformed input raises gdb::Error.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gdb {

// Building or consuming gdb.dll. Define GDB_SHARED in your project to link the
// DLL; leave it undefined to use the static library or the merged source files.
// GDB_BUILD is set only while compiling the DLL itself.
#if defined(_WIN32) && defined(GDB_SHARED)
#  if defined(GDB_BUILD)
#    define GDB_API __declspec(dllexport)
#  else
#    define GDB_API __declspec(dllimport)
#  endif
#else
#  define GDB_API
#endif

using Id = std::uint64_t;

inline constexpr Id kNoId = static_cast<Id>(~static_cast<Id>(0));
inline constexpr std::uint32_t kNoStr = 0xFFFFFFFFu;

// Ceiling on intermediate rows a single query may materialise. Traversal depth
// is already bounded by the query text (every hop is written out), but the row
// count is not: two hops on a dense graph is degree squared, three is cubed, and
// LIMIT does not help because it is applied after the rows exist. Crossing this
// raises an error instead of exhausting memory.
inline constexpr std::size_t kDefaultRowLimit = 1000000;

// ---------------------------------------------------------------------------
// errors
// ---------------------------------------------------------------------------

class GDB_API Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

// ---------------------------------------------------------------------------
// values
// ---------------------------------------------------------------------------

// The engine deals in exactly three kinds of value: text, a list of text, and a
// reference to a node or a relationship. There is no numeric or boolean type
// anywhere, not even for what the engine itself produces -- count() and id()
// yield text too, so that `WHERE id(n) = 0` and `WHERE id(n) = '0'` agree
// instead of quietly never matching.
//
// The one literal that is not text is null, because `SET n.key = null` is the
// standard Cypher way to remove a property and `IS NULL` has to be able to ask
// whether a key is present at all. Quote it ('null') to store those four
// characters.
enum class ValueType : std::uint8_t { Null, Str, List, Node, Rel };

class Database;

struct GDB_API Value {
    ValueType type = ValueType::Null;
    std::string s;                   // Str
    std::vector<std::string> items;  // List, always a list of strings
    Id ref = kNoId;                  // Node / Rel

    Value() = default;

    static Value null();
    static Value text(std::string v);
    static Value list(std::vector<std::string> v);
    static Value node(Id id);
    static Value rel(Id id);

    bool isNull() const noexcept { return type == ValueType::Null; }
    bool isGraph() const noexcept { return type == ValueType::Node || type == ValueType::Rel; }

    // Caller conveniences. The engine itself never parses property text.
    std::int64_t asInt() const;
    bool asBool() const;
    std::string asText() const;    // List joins with ", "
    std::string toString() const;  // canonical form, used for indexing and ordering

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }
};

struct GDB_API Prop {
    std::string key;
    Value value;
};

using Properties = std::vector<Prop>;
using Parameters = std::map<std::string, Value>;

// ---------------------------------------------------------------------------
// query results
// ---------------------------------------------------------------------------

struct GDB_API Row {
    std::vector<Value> values;
};

struct GDB_API QueryResult {
    std::vector<std::string> columns;
    std::vector<Row> rows;

    std::int64_t nodes_created = 0;
    std::int64_t nodes_deleted = 0;
    std::int64_t edges_created = 0;
    std::int64_t edges_deleted = 0;
    std::int64_t props_set = 0;

    std::size_t size() const noexcept { return rows.size(); }
    bool empty() const noexcept { return rows.empty(); }

    const Value& at(std::size_t row, std::size_t column) const;
    const Value& at(std::size_t row, const std::string& column) const;

    // Human readable table, graph references expanded through `db`.
    std::string dump(const Database& db) const;
};

struct GDB_API EdgeRef {
    Id id = kNoId;
    std::string type;
    Id src = kNoId;
    Id dst = kNoId;
};

// ---------------------------------------------------------------------------
// storage (defined in src/internal.h)
// ---------------------------------------------------------------------------

class Store;

// ---------------------------------------------------------------------------
// database
// ---------------------------------------------------------------------------

class GDB_API Database {
public:
    Database();
    ~Database();

    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // --- persistence -------------------------------------------------------
    // load() with a missing path yields an empty database, not an error.
    static Database open(const std::string& path);
    void load(const std::string& path);

    // Writes the snapshot. Refuses when the file has been changed by another
    // writer since it was loaded, rather than silently discarding that work;
    // pass force to overwrite anyway. The replacement is atomic and flushed.
    void save(const std::string& path, bool force = false);

    // Generation of the file this database was loaded from or last saved as.
    std::uint64_t generation() const noexcept;

    // --- direct write API --------------------------------------------------
    Id addNode(const std::string& label, const Properties& props = {});
    Id addEdge(const std::string& type, Id src, Id dst, const Properties& props = {});
    bool removeNode(Id id);
    bool removeEdge(Id id);

    bool setNodeProp(Id id, const std::string& key, const Value& value);
    bool setEdgeProp(Id id, const std::string& key, const Value& value);
    bool removeNodeProp(Id id, const std::string& key);
    bool removeEdgeProp(Id id, const std::string& key);

    // --- direct read API ---------------------------------------------------
    bool nodeExists(Id id) const noexcept;
    bool edgeExists(Id id) const noexcept;
    std::size_t nodeCount() const noexcept;
    std::size_t edgeCount() const noexcept;

    // Records left behind by deletion. A snapshot keeps them so that ids stay
    // stable across a save; call compact() to reclaim them.
    std::size_t tombstoneCount() const noexcept;

    // Rough resident footprint of the in-memory graph, for reporting and for
    // warnings. Derived from the actual arrays, so it tracks property count.
    std::size_t estimatedBytes() const noexcept;

    // Escape hatch for kDefaultRowLimit.
    void setRowLimit(std::size_t rows) noexcept;
    std::size_t rowLimit() const noexcept;

    std::string nodeLabel(Id id) const;
    std::string edgeType(Id id) const;
    Id edgeSource(Id id) const;
    Id edgeTarget(Id id) const;

    Value nodeProp(Id id, const std::string& key) const;
    Value edgeProp(Id id, const std::string& key) const;
    Properties nodeProps(Id id) const;
    Properties edgeProps(Id id) const;
    bool nodeHasProp(Id id, const std::string& key) const;

    std::vector<Id> allNodes() const;
    std::vector<Id> allEdges() const;
    std::vector<Id> nodesWithLabel(const std::string& label) const;
    std::vector<EdgeRef> outEdges(Id id) const;
    std::vector<EdgeRef> inEdges(Id id) const;
    std::vector<EdgeRef> edgesBetween(Id src, Id dst) const;

    // --- formatting --------------------------------------------------------
    std::string formatNode(Id id) const;
    std::string formatEdge(Id id) const;
    std::string formatValue(const Value& value) const;

    // --- Cypher ------------------------------------------------------------
    QueryResult query(const std::string& cypher);
    QueryResult query(const std::string& cypher, const Parameters& params);

    // --- escape hatch ------------------------------------------------------
    Store& store() noexcept;
    const Store& store() const noexcept;

private:
    std::unique_ptr<Store> store_;
    std::size_t row_limit_ = kDefaultRowLimit;
};

}  // namespace gdb

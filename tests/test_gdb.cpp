// test_gdb.cpp - self-contained checks. No test framework, no dependencies.
//
// Every check is a real end-to-end assertion: build a graph through Cypher,
// read it back through Cypher, persist it, reload it, and compare.

#if defined(_MSC_VER)
// This file deliberately uses plain fopen/fread for the truncation check.
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "gdb/gdb.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n", line, what.c_str());
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(a, b)                                              \
    do {                                                            \
        const auto lhs = (a);                                       \
        const auto rhs = (b);                                       \
        check(lhs == rhs, std::string(#a) + " == " + #b, __LINE__); \
    } while (0)

void section(const char* name) { std::printf("- %s\n", name); }

std::int64_t asInt(const gdb::QueryResult& r, std::size_t row = 0, std::size_t col = 0) {
    return r.at(row, col).asInt();
}

std::string asText(const gdb::QueryResult& r, std::size_t row = 0, std::size_t col = 0) {
    return r.at(row, col).asText();
}

void testCreateAndMatch(gdb::Database& db) {
    section("CREATE / MATCH / RETURN");

    auto created = db.query(
        "CREATE (a:Person {name: 'Ada', age: 36})-[:KNOWS {since: 1843}]->"
        "(b:Person {name: 'Charles', age: 33}) RETURN a, b");
    CHECK_EQ(created.nodes_created, 2);
    CHECK_EQ(created.edges_created, 1);
    CHECK_EQ(created.size(), 1u);

    auto byLabel = db.query("MATCH (p:Person) RETURN p ORDER BY p.name");
    CHECK_EQ(byLabel.size(), 2u);
    CHECK_EQ(asText(byLabel, 0, 0), std::string("#0"));
    CHECK_EQ(asText(byLabel, 1, 0), std::string("#1"));

    auto byProp = db.query("MATCH (p:Person {name: 'Ada'}) RETURN p.age");
    CHECK_EQ(byProp.size(), 1u);
    CHECK_EQ(asInt(byProp), 36);

    auto alone = db.query("MATCH (p {name: 'Charles'}) RETURN p.name AS n");
    CHECK_EQ(alone.size(), 1u);
    CHECK_EQ(asText(alone), std::string("Charles"));
    CHECK_EQ(alone.columns.at(0), std::string("n"));
}

void testTraversal(gdb::Database& db) {
    section("traversal");
    db.query("CREATE (a:City {name: 'Rome'})-[:ROAD {km: 500}]->(b:City {name: 'Milan'})"
             "-[:ROAD {km: 280}]->(c:City {name: 'Turin'})");
    db.query("CREATE (c:City {name: 'Genoa'})");
    db.query("MATCH (a:City {name: 'Milan'}), (g:City {name: 'Genoa'}) CREATE (a)-[:ROAD {km: 130}]->(g)");

    auto oneHop = db.query("MATCH (:City {name: 'Rome'})-[:ROAD]->(x) RETURN x.name");
    CHECK_EQ(oneHop.size(), 1u);
    CHECK_EQ(asText(oneHop), std::string("Milan"));

    auto twoHop = db.query("MATCH (a:City {name: 'Rome'})-[r1:ROAD]->(b)-[r2:ROAD]->(c) "
                           "RETURN b.name AS via, c.name AS dest ORDER BY c.name");
    CHECK_EQ(twoHop.size(), 2u);
    CHECK_EQ(asText(twoHop, 0, 1), std::string("Genoa"));
    CHECK_EQ(asText(twoHop, 1, 1), std::string("Turin"));

    auto backwards = db.query("MATCH (x)-[:ROAD]->(:City {name: 'Turin'}) RETURN x.name");
    CHECK_EQ(backwards.size(), 1u);
    CHECK_EQ(asText(backwards), std::string("Milan"));

    // Milan links to Rome, Turin and Genoa, and an undirected pattern sees all three.
    auto undirected = db.query("MATCH (x:City {name: 'Milan'})-[:ROAD]-(y) RETURN y.name ORDER BY y.name");
    CHECK_EQ(undirected.size(), 3u);

    auto edgeProp = db.query("MATCH (:City {name: 'Rome'})-[r:ROAD]->() RETURN r.km");
    CHECK_EQ(asInt(edgeProp), 500);

    auto viaEdgeVar = db.query("MATCH (a:City)-[r:ROAD]->(b:City) WHERE r.km > 400 RETURN a.name, b.name");
    CHECK_EQ(viaEdgeVar.size(), 1u);
    CHECK_EQ(asText(viaEdgeVar, 0, 0), std::string("Rome"));
}

void testWhereOperators(gdb::Database& db) {
    section("WHERE and operators");
    auto ageFilter = db.query("MATCH (p:Person) WHERE p.age >= 34 RETURN p.name");
    CHECK_EQ(ageFilter.size(), 1u);
    CHECK_EQ(asText(ageFilter), std::string("Ada"));

    auto combined = db.query(
        "MATCH (p:Person) WHERE p.name CONTAINS 'a' AND (p.age < 35 OR p.name = 'Ada') "
        "RETURN p.name ORDER BY p.name");
    CHECK_EQ(combined.size(), 2u);

    auto negation = db.query("MATCH (p:Person) WHERE NOT p.name = 'Ada' RETURN p.name");
    CHECK_EQ(negation.size(), 1u);
    CHECK_EQ(asText(negation), std::string("Charles"));

    auto starts = db.query("MATCH (p:Person) WHERE p.name STARTS WITH 'Ch' RETURN p.name");
    CHECK_EQ(starts.size(), 1u);
    CHECK_EQ(asText(starts), std::string("Charles"));

    auto ends = db.query("MATCH (p:Person) WHERE p.name ENDS WITH 'les' RETURN p.name");
    CHECK_EQ(ends.size(), 1u);

    auto notContains = db.query("MATCH (p:Person) WHERE p.name NOT CONTAINS 'd' RETURN count(*) AS n");
    CHECK_EQ(asInt(notContains), 1);

    auto nullCheck = db.query("MATCH (p:Person) WHERE p.email IS NULL RETURN count(*) AS n");
    CHECK_EQ(asInt(nullCheck), 2);

    auto notNull = db.query("MATCH (p:Person) WHERE p.email IS NOT NULL RETURN count(*) AS n");
    CHECK_EQ(asInt(notNull), 0);

    auto identity = db.query("MATCH (p:Person {name: 'Ada'}) RETURN id(p) AS i, labels(p) AS l");
    CHECK_EQ(asInt(identity, 0, 0), 0);
    CHECK_EQ(asText(identity, 0, 1), std::string("Person"));

    auto typeFn = db.query(
        "MATCH (:Person {name: 'Ada'})-[r:KNOWS]->(b) RETURN type(r) AS t, b.name AS n");
    CHECK_EQ(asText(typeFn, 0, 0), std::string("KNOWS"));
    CHECK_EQ(asText(typeFn, 0, 1), std::string("Charles"));

    gdb::Parameters params;
    params["who"] = gdb::Value::text("Charles");
    auto viaParam = db.query("MATCH (p:Person {name: $who}) RETURN p.age", params);
    CHECK_EQ(asInt(viaParam), 33);
}

void testCounting(gdb::Database& db) {
    section("count");
    auto counted = db.query("MATCH (p:Person) RETURN count(*) AS all, count(p.email) AS withEmail");
    CHECK_EQ(counted.size(), 1u);
    CHECK_EQ(asInt(counted, 0, 0), 2);
    CHECK_EQ(asInt(counted, 0, 1), 0);

    // An aggregate over an empty match still yields exactly one row.
    auto none = db.query("MATCH (p:Person {name: 'nobody'}) RETURN count(*) AS n");
    CHECK_EQ(none.size(), 1u);
    CHECK_EQ(asInt(none), 0);
}

void testOrderingAndPaging(gdb::Database& db) {
    section("ordering and paging");
    // Property values are text, so ordering is text ordering. These are the two
    // facts that follow, and they are pinned here on purpose: zero-padded values
    // (ISO dates) order correctly, unpadded numbers do not.
    db.query("CREATE (s:Score {team: 'red', points: 10})");
    db.query("CREATE (s:Score {team: 'red', points: 20})");
    db.query("CREATE (s:Score {team: 'blue', points: 5})");
    db.query("CREATE (s:Date {day: '1990-03-05', tag: 'a'})");
    db.query("CREATE (s:Date {day: '2026-01-02', tag: 'b'})");
    db.query("CREATE (s:Date {day: '1971-12-31', tag: 'c'})");

    // Lexicographic, not numeric: "5" sorts after "20".
    auto ascending = db.query("MATCH (s:Score) RETURN s.points AS p ORDER BY s.points");
    CHECK_EQ(ascending.size(), 3u);
    CHECK_EQ(asText(ascending, 0, 0), std::string("10"));
    CHECK_EQ(asText(ascending, 1, 0), std::string("20"));
    CHECK_EQ(asText(ascending, 2, 0), std::string("5"));

    // ISO dates are zero-padded, so text order is chronological order.
    auto byDate = db.query("MATCH (d:Date) RETURN d.tag AS t ORDER BY d.day");
    CHECK_EQ(byDate.size(), 3u);
    CHECK_EQ(asText(byDate, 0, 0), std::string("c"));  // 1971
    CHECK_EQ(asText(byDate, 1, 0), std::string("a"));  // 1990
    CHECK_EQ(asText(byDate, 2, 0), std::string("b"));  // 2026

    auto limited = db.query("MATCH (s:Score) RETURN s.points AS p ORDER BY s.points LIMIT 2");
    CHECK_EQ(limited.size(), 2u);
    CHECK_EQ(asText(limited, 1, 0), std::string("20"));

    auto byAlias = db.query("MATCH (s:Score) RETURN s.team AS team ORDER BY team LIMIT 1");
    CHECK_EQ(asText(byAlias), std::string("blue"));

    auto filtered = db.query("MATCH (s:Score) WHERE s.team = 'red' RETURN count(*) AS n");
    CHECK_EQ(asInt(filtered), 2);

    auto star = db.query("MATCH (s:Score) RETURN * LIMIT 1");
    CHECK_EQ(star.columns.size(), 1u);  // RETURN * gives the bound variables
    CHECK_EQ(star.columns.at(0), std::string("s"));
}

void testWriteBack(gdb::Database& db) {
    section("SET / DELETE");
    db.query("CREATE (t:Task {title: 'write db', done: false})");
    auto set = db.query("MATCH (t:Task {title: 'write db'}) SET t.done = true RETURN t.done");
    CHECK_EQ(set.size(), 1u);
    CHECK_EQ(set.at(0, 0).asText(), std::string("true"));

    auto verify = db.query("MATCH (t:Task) WHERE t.done = true RETURN count(*) AS n");
    CHECK_EQ(asInt(verify), 1);

    db.query("CREATE (x:Temp {name: 'gone'})-[:LINK]->(y:Temp {name: 'also gone'})");
    const std::size_t before = db.edgeCount();
    auto killed = db.query("MATCH (x:Temp {name: 'gone'}) DELETE x");
    CHECK_EQ(killed.nodes_deleted, 1);
    CHECK_EQ(killed.edges_deleted, 1);
    CHECK_EQ(db.edgeCount(), before - 1);

    auto left = db.query("MATCH (x:Temp) RETURN count(*) AS n");
    CHECK_EQ(asInt(left), 1);

    auto relDelete = db.query("MATCH (:Score)-[r]->() DELETE r RETURN count(*) AS removed");
    CHECK_EQ(relDelete.edges_deleted, 0);
}

void testPersistence(gdb::Database& db) {
    section("persistence round trip");

    std::filesystem::path path = std::filesystem::temp_directory_path() / "gdb_roundtrip.gdb";
    std::filesystem::remove(path);
    db.save(path.string());
    CHECK(std::filesystem::exists(path));

    gdb::Database reloaded = gdb::Database::open(path.string());
    CHECK_EQ(reloaded.nodeCount(), db.nodeCount());
    CHECK_EQ(reloaded.edgeCount(), db.edgeCount());

    auto names = reloaded.query("MATCH (p:Person) RETURN p.name ORDER BY p.name");
    CHECK_EQ(names.size(), 2u);
    CHECK_EQ(asText(names, 0, 0), std::string("Ada"));

    auto traverse = reloaded.query("MATCH (:City {name: 'Rome'})-[:ROAD]->(x) RETURN x.name");
    CHECK_EQ(asText(traverse), std::string("Milan"));

    // Indexes must be rebuilt on load, not just the arrays.
    auto indexed = reloaded.query("MATCH (p {name: 'Charles'}) RETURN p.age");
    CHECK_EQ(asInt(indexed), 33);

    // Mutating the reloaded copy must not touch the file until it is saved.
    reloaded.query("CREATE (:Marker {tag: 'roundtrip'})");
    gdb::Database reopened = gdb::Database::open(path.string());
    auto markers = reopened.query("MATCH (m:Marker) RETURN count(*) AS n");
    CHECK_EQ(asInt(markers), 0);

    // Truncation must be detected rather than parsed as garbage.
    std::filesystem::path broken = std::filesystem::temp_directory_path() / "gdb_broken.gdb";
    std::filesystem::remove(broken);
    {
        const std::uintmax_t full = std::filesystem::file_size(path);
        std::FILE* src = std::fopen(path.string().c_str(), "rb");
        std::FILE* dst = std::fopen(broken.string().c_str(), "wb");
        std::vector<char> head(static_cast<std::size_t>(full / 2));
        const std::size_t n = std::fread(head.data(), 1, head.size(), src);
        std::fwrite(head.data(), 1, n, dst);
        std::fclose(src);
        std::fclose(dst);
    }
    bool threw = false;
    try {
        gdb::Database::open(broken.string());
    } catch (const gdb::Error&) {
        threw = true;
    }
    CHECK(threw);

    std::filesystem::remove(path);
    std::filesystem::remove(broken);
}

void testRebuildIsLossless() {
    section("rebuilding through the public API is a safe compaction");

    gdb::Database src;
    src.query(
        "CREATE (a:人物 {name: '张三', 昵称: '老张'})-[:朋友 {since: 1990}]->"
        "(b:人物 {name: '李四'})");
    src.query("CREATE (:知识点 {name: 'Cypher'})");
    src.query("CREATE (:临时 {x: 1})");
    src.query("CREATE (:临时 {x: 2})");
    src.query("MATCH (t:临时) DELETE t");  // leaves tombstones behind
    CHECK(src.tombstoneCount() > 0);

    // Exactly what `gdb_cli compact` does: copy through the public API, then
    // write a fresh database. Nothing here can renumber ids under a caller,
    // because the caller is the tool and there is no caller left holding any.
    std::map<gdb::Id, gdb::Id> remap;
    gdb::Database rebuilt;
    for (gdb::Id id : src.allNodes()) {
        remap[id] = rebuilt.addNode(src.nodeLabel(id), src.nodeProps(id));
    }
    for (gdb::Id id : src.allEdges()) {
        rebuilt.addEdge(src.edgeType(id), remap[src.edgeSource(id)], remap[src.edgeTarget(id)],
                        src.edgeProps(id));
    }

    CHECK_EQ(rebuilt.nodeCount(), src.nodeCount());
    CHECK_EQ(rebuilt.edgeCount(), src.edgeCount());
    CHECK_EQ(rebuilt.tombstoneCount(), 0u);
    CHECK(rebuilt.estimatedBytes() < src.estimatedBytes());

    auto names = rebuilt.query("MATCH (p:人物) RETURN p.name AS n ORDER BY n");
    CHECK_EQ(names.size(), 2u);
    CHECK_EQ(asText(names, 0, 0), std::string("张三"));

    auto detail = rebuilt.query(
        "MATCH (:人物 {name: '张三'})-[r:朋友]->(b) RETURN r.since AS s, b.name AS n");
    CHECK_EQ(detail.size(), 1u);
    CHECK_EQ(asInt(detail, 0, 0), 1990);
    CHECK_EQ(asText(detail, 0, 1), std::string("李四"));
    CHECK_EQ(asText(rebuilt.query("MATCH (p:人物 {name: '张三'}) RETURN p.昵称 AS n"), 0, 0),
             std::string("老张"));
}

void testErrors() {
    section("errors are raised, never swallowed");
    gdb::Database db;
    db.query("CREATE (:Thing {name: 'x'})");

    auto expectError = [&](const std::string& cypher) {
        bool threw = false;
        try {
            db.query(cypher);
        } catch (const gdb::Error&) {
            threw = true;
        }
        check(threw, "expected an error for: " + cypher, __LINE__);
    };

    expectError("MATCH (n RETURN n");
    expectError("MATCH (n) RETURN n LIMIT");
    expectError("MATCH (n) RETURN m");
    expectError("MATCH (n) RETURN n ORDER BY n.name DESCENDING WRONG");
    expectError("MATCH (n) RETURN n WITH n");
    expectError("MATCH p = (n)-->(m) RETURN p");
    expectError("MATCH (a)-[:X|:Y]->(b) RETURN a");
    expectError("MATCH (n:Thing:Other) RETURN n");
    expectError("MATCH (n) RETURN n.name + ");
    expectError("CREATE (n:Thing {name: $missing})");

    // Deliberately outside the dialect. These must fail with a message that
    // says what to write instead, because the caller is a language model.
    expectError("MATCH (n) RETURN toUpper(n.name)");
    expectError("MATCH (n) RETURN sum(n.v)");
    expectError("MATCH (n) RETURN CASE WHEN n.v = 1 THEN 2 END");
    expectError("OPTIONAL MATCH (n)-->(m) RETURN n");
    expectError("MATCH (n) WITH n RETURN n");
    expectError("UNWIND [1, 2] AS x RETURN x");
    expectError("MATCH (n)-[:R*1..2]->(m) RETURN n");
    expectError("MERGE (a:Thing)-[:R]->(b:Thing) ON CREATE SET a.v = 1");
    // SKIP without a defined order would silently return overlapping pages.
    expectError("MATCH (n) RETURN n SKIP 1");
    expectError("MATCH (n) RETURN n LIMIT 5 SKIP 1");

    CHECK_EQ(db.nodeCount(), 1u);
}

void testUtf8() {
    section("UTF-8 is stored byte for byte");

    // These literals are compiled in as UTF-8 (the build passes /utf-8), so they
    // never pass through a shell or a code page. What goes in must come out.
    const std::string chinese = "语义记忆：知识库";
    const std::string emoji = "🧠";  // four UTF-8 bytes
    const std::string mixed = "ASCII + 中文 + ünïcödé + 🧠 + \t \"quoted\" \\slash";

    gdb::Database db;
    const gdb::Id a = db.addNode("记忆", {{"text", gdb::Value::text(chinese)},
                                          {"note", gdb::Value::text(emoji)}});
    db.addNode("记忆", {{"text", gdb::Value::text(mixed)}});

    CHECK_EQ(db.nodeProp(a, "text").s, chinese);
    CHECK_EQ(db.nodeProp(a, "note").s, emoji);
    CHECK_EQ(db.nodeLabel(a), std::string("记忆"));
    CHECK_EQ(db.nodeProp(a, "text").s.size(), std::string("语义记忆：知识库").size());

    auto byLabel = db.query("MATCH (m:记忆) RETURN count(*) AS n");
    CHECK_EQ(asInt(byLabel), 2);

    auto exact = db.query("MATCH (m:记忆 {text: '语义记忆：知识库'}) RETURN m.text AS t");
    CHECK_EQ(exact.size(), 1u);
    CHECK_EQ(asText(exact), chinese);

    auto contains = db.query("MATCH (m:记忆) WHERE m.text CONTAINS '知识库' RETURN count(*) AS n");
    CHECK_EQ(asInt(contains), 1);

    auto emojiHit = db.query("MATCH (m:记忆) WHERE m.note = '🧠' RETURN m.text AS t");
    CHECK_EQ(emojiHit.size(), 1u);
    CHECK_EQ(asText(emojiHit), chinese);

    std::filesystem::path path = std::filesystem::temp_directory_path() / "gdb_utf8.gdb";
    std::filesystem::remove(path);
    db.save(path.string());
    gdb::Database back = gdb::Database::open(path.string());

    CHECK_EQ(back.nodeProp(a, "text").s, chinese);
    CHECK_EQ(back.nodeProp(a, "note").s, emoji);
    CHECK_EQ(back.nodeLabel(a), std::string("记忆"));

    auto reread = back.query("MATCH (m:记忆) WHERE m.text CONTAINS '🧠' RETURN count(*) AS n");
    CHECK_EQ(asInt(reread), 1);
    auto rereadMixed = back.query("MATCH (m:记忆 {text: $t}) RETURN count(*) AS n",
                                  {{"t", gdb::Value::text(mixed)}});
    CHECK_EQ(asInt(rereadMixed), 1);
    std::filesystem::remove(path);
}

void testWriterConflict() {
    section("two writers, durability, atomic replace");

    std::filesystem::path path = std::filesystem::temp_directory_path() / "gdb_conflict.gdb";
    std::filesystem::remove(path);

    gdb::Database a;
    a.query("CREATE (:人物 {name: '张三'})");
    a.save(path.string());
    CHECK_EQ(a.generation(), 1ull);
    CHECK(std::filesystem::exists(path));
    // The staging file must not survive a successful save.
    CHECK(!std::filesystem::exists(path.string() + ".tmp"));

    gdb::Database b = gdb::Database::open(path.string());
    CHECK_EQ(b.generation(), 1ull);

    // a commits first, which is what the nightly job does.
    a.query("CREATE (:人物 {name: '李四'})");
    a.save(path.string());
    CHECK_EQ(a.generation(), 2ull);

    // b is now stale. It must be refused, not silently discard a night of work.
    b.query("CREATE (:人物 {name: '王五'})");
    bool refused = false;
    try {
        b.save(path.string());
    } catch (const gdb::Error&) {
        refused = true;
    }
    CHECK(refused);

    gdb::Database survived = gdb::Database::open(path.string());
    auto names = survived.query("MATCH (p:人物) RETURN p.name ORDER BY p.name");
    CHECK_EQ(names.size(), 2u);
    CHECK_EQ(asText(names, 0, 0), std::string("张三"));

    // Reload and re-apply is the supported recovery.
    b.load(path.string());
    b.query("CREATE (:人物 {name: '王五'})");
    b.save(path.string());
    CHECK_EQ(b.generation(), 3ull);

    gdb::Database merged = gdb::Database::open(path.string());
    CHECK_EQ(asInt(merged.query("MATCH (p:人物) RETURN count(*) AS n")), 3);
    CHECK_EQ(merged.generation(), 3ull);

    // Writing over a file this database never loaded needs an explicit force.
    gdb::Database blind;
    blind.query("CREATE (:人物 {name: '赵六'})");
    bool blindRefused = false;
    try {
        blind.save(path.string());
    } catch (const gdb::Error&) {
        blindRefused = true;
    }
    CHECK(blindRefused);

    blind.save(path.string(), true);
    CHECK_EQ(blind.generation(), 4ull);
    gdb::Database forced = gdb::Database::open(path.string());
    CHECK_EQ(asInt(forced.query("MATCH (p:人物) RETURN count(*) AS n")), 1);

    // Repeated saves must keep working and keep the file present at every step.
    for (int i = 0; i < 5; ++i) {
        forced.query("CREATE (:节拍 {n: " + std::to_string(i) + "})");
        forced.save(path.string());
        CHECK(std::filesystem::exists(path));
        CHECK(!std::filesystem::exists(path.string() + ".tmp"));
    }
    CHECK_EQ(forced.generation(), 9ull);
    CHECK_EQ(gdb::Database::open(path.string()).generation(), 9ull);

    std::filesystem::remove(path);
}

void testMergeAndCrud() {
    section("MERGE, SET maps, IN, arithmetic, DISTINCT, SKIP");

    gdb::Database db;

    // MERGE creates when absent, binds when present.
    auto first = db.query("MERGE (p:人物 {name: '张三'}) RETURN p.name AS n");
    CHECK_EQ(first.nodes_created, 1);
    CHECK_EQ(first.size(), 1u);

    // The nightly job may run twice. Idempotence is the whole point.
    for (int i = 0; i < 5; ++i) db.query("MERGE (p:人物 {name: '张三'})");
    CHECK_EQ(db.nodeCount(), 1u);

    db.query("MERGE (a:人物 {name: '张三'}) MERGE (b:人物 {name: '李四'}) MERGE (a)-[:朋友]->(b)");
    CHECK_EQ(db.nodeCount(), 2u);
    CHECK_EQ(db.edgeCount(), 1u);
    db.query("MERGE (a:人物 {name: '张三'}) MERGE (b:人物 {name: '李四'}) MERGE (a)-[:朋友]->(b)");
    CHECK_EQ(db.edgeCount(), 1u);

    // WHERE is pushed into the traversal, so it must only be evaluated once
    // every variable it mentions is bound -- here, from two patterns.
    auto pairs = db.query(
        "MATCH (a:人物 {name: '张三'}), (b:人物 {name: '李四'}) "
        "WHERE a.name = '张三' AND b.name = '李四' RETURN a.name AS x, b.name AS y");
    CHECK_EQ(pairs.size(), 1u);
    CHECK_EQ(asText(pairs, 0, 0), std::string("张三"));
    CHECK_EQ(asText(pairs, 0, 1), std::string("李四"));

    // And a WHERE that filters everything out must yield nothing, not a stray row.
    CHECK_EQ(db.query("MATCH (a:人物 {name: '张三'}) WHERE a.name = '不存在' RETURN a").size(), 0u);

    // An ambiguous key must fail loudly instead of picking a node at random.
    db.query("CREATE (:人物 {name: '王五'})");
    db.query("CREATE (:人物 {name: '王五'})");
    bool ambiguous = false;
    try {
        db.query("MERGE (p:人物 {name: '王五'})");
    } catch (const gdb::Error&) {
        ambiguous = true;
    }
    CHECK(ambiguous);

    // An unbound endpoint is merged part by part, never duplicated. This is the
    // form a language model writes naturally, so it has to work.
    db.query("MERGE (a:人物 {name: '张三'})-[:同事]->(b:人物 {name: '赵六'})");
    CHECK_EQ(db.nodeCount(), 5u);  // 张三, 李四, 王五, 王五, 赵六
    CHECK_EQ(db.edgeCount(), 2u);
    db.query("MERGE (a:人物 {name: '张三'})-[:同事]->(b:人物 {name: '赵六'})");
    CHECK_EQ(db.nodeCount(), 5u);  // and it stays idempotent
    CHECK_EQ(db.edgeCount(), 2u);

    // SET with a map replaces everything.
    db.query("MATCH (p:人物 {name: '张三'}) SET p = {name: '张三', 昵称: '老张', hits: 5}");
    auto replaced = db.query(
        "MATCH (p:人物 {name: '张三'}) RETURN p.昵称 AS nick, p.hits AS s, p.email AS e");
    CHECK_EQ(asText(replaced, 0, 0), std::string("老张"));
    CHECK_EQ(asInt(replaced, 0, 1), 5);
    CHECK(replaced.at(0, 2).isNull());

    // += merges into what is already there.
    db.query("MATCH (p:人物 {name: '张三'}) SET p += {生日: '1990-03-05', hits: 9}");
    auto merged = db.query(
        "MATCH (p:人物 {name: '张三'}) RETURN p.昵称 AS nick, p.生日 AS b, p.hits AS s");
    CHECK_EQ(asText(merged, 0, 0), std::string("老张"));
    CHECK_EQ(asText(merged, 0, 1), std::string("1990-03-05"));
    CHECK_EQ(asInt(merged, 0, 2), 9);

    auto inList = db.query(
        "MATCH (p:人物) WHERE p.name IN ['张三', '李四'] RETURN p.name ORDER BY p.name");
    CHECK_EQ(inList.size(), 2u);
    CHECK_EQ(asInt(db.query("MATCH (p:人物) WHERE p.name NOT IN ['张三'] RETURN count(*) AS n")), 4);

    // Numbers and booleans are stored as the text that was written.
    db.query("MATCH (p:人物 {name: '张三'}) SET p.年龄 = 36, p.已婚 = false, p.零 = -1, p.比率 = 0.1234");
    auto literals = db.query(
        "MATCH (p:人物 {name: '张三'}) RETURN p.年龄 AS a, p.已婚 AS m, p.零 AS z, p.比率 AS r");
    CHECK_EQ(asText(literals, 0, 0), std::string("36"));
    CHECK_EQ(asText(literals, 0, 1), std::string("false"));
    CHECK_EQ(asText(literals, 0, 2), std::string("-1"));
    CHECK_EQ(asText(literals, 0, 3), std::string("0.1234"));

    // count() and id() are text as well, so comparing them against a literal
    // works whether or not it is quoted. While they were numbers, every such
    // comparison silently matched nothing instead of failing.
    CHECK_EQ(db.query("MATCH (p:人物 {name: '张三'}) RETURN id(p) AS i").at(0, 0).type,
             gdb::ValueType::Str);
    CHECK_EQ(db.query("MATCH (p:人物 {name: '张三'}) WHERE id(p) = 0 RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             1);
    CHECK_EQ(db.query("MATCH (p:人物 {name: '张三'}) WHERE id(p) = '0' RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             1);
    CHECK_EQ(db.query("MATCH (p:人物) WHERE id(p) > '0' RETURN count(*) AS n").at(0, 0).asInt(), 4);

    // '+' concatenates text. There is no arithmetic on properties, because their
    // values are text -- the error says exactly that.
    auto concat = db.query("RETURN 'x' + 'y' AS d");
    CHECK_EQ(asText(concat, 0, 0), std::string("xy"));
    bool noArithmetic = false;
    try {
        db.query("MATCH (p:人物 {name: '张三'}) SET p.hits = p.hits - 1");
    } catch (const gdb::Error& e) {
        noArithmetic = std::string(e.what()).find("property values are text") != std::string::npos;
    }
    CHECK(noArithmetic);

    // A key can hold several values: this is what value(s) in the model means.
    db.query("MATCH (p:人物 {name: '张三'}) SET p.兴趣 = ['天文学', '编程']");
    auto interests = db.query("MATCH (p:人物 {name: '张三'}) RETURN p.兴趣 AS list");
    CHECK_EQ(interests.at(0, 0).type, gdb::ValueType::List);
    CHECK_EQ(interests.at(0, 0).items.size(), 2u);
    CHECK_EQ(asText(interests, 0, 0), std::string("天文学, 编程"));

    // Appending keeps what was there.
    db.query("MATCH (p:人物 {name: '张三'}) SET p.兴趣 = p.兴趣 + ['音乐']");
    CHECK_EQ(db.query("MATCH (p:人物 {name: '张三'}) RETURN p.兴趣").at(0, 0).items.size(), 3u);

    // Membership against a multi-valued property, which is the point of it.
    CHECK_EQ(db.query("MATCH (p:人物) WHERE '编程' IN p.兴趣 RETURN count(*) AS n").at(0, 0).asInt(), 1);
    CHECK_EQ(db.query("MATCH (p:人物) WHERE '音乐' IN p.兴趣 RETURN count(*) AS n").at(0, 0).asInt(), 1);
    CHECK_EQ(db.query("MATCH (p:人物) WHERE '化学' IN p.兴趣 RETURN count(*) AS n").at(0, 0).asInt(), 0);

    // Fragment search is what the AI actually does, and it has to test each
    // element rather than the joined text: '学, 编' spans two elements and must
    // not match.
    CHECK_EQ(db.query("MATCH (p:人物) WHERE p.兴趣 CONTAINS '天文' RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             1);
    CHECK_EQ(db.query("MATCH (p:人物) WHERE p.兴趣 CONTAINS '学, 编' RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             0);
    CHECK_EQ(db.query("MATCH (p:人物) WHERE p.兴趣 STARTS WITH '编' RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             1);
    CHECK_EQ(db.query("MATCH (p:人物) WHERE p.兴趣 ENDS WITH '学' RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             1);
    // And single-valued text still behaves as before.
    CHECK_EQ(db.query("MATCH (p:人物) WHERE p.name CONTAINS '张' RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             1);

    // Waiting on a value list survives a save and reload byte for byte.
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "gdb_list.gdb";
        std::filesystem::remove(path);
        db.save(path.string());
        gdb::Database back = gdb::Database::open(path.string());
        auto reloaded = back.query("MATCH (p:人物 {name: '张三'}) RETURN p.兴趣 AS l");
        CHECK_EQ(reloaded.at(0, 0).type, gdb::ValueType::List);
        CHECK_EQ(reloaded.at(0, 0).items.size(), 3u);
        CHECK_EQ(reloaded.at(0, 0).items.at(2), std::string("音乐"));
        std::filesystem::remove(path);
    }

    // Writing a list over a list replaces it wholesale, like any other property.
    db.query("MATCH (p:人物 {name: '张三'}) SET p.兴趣 = ['只剩一个']");
    CHECK_EQ(db.query("MATCH (p:人物 {name: '张三'}) RETURN p.兴趣").at(0, 0).items.size(), 1u);

    // null still removes a property: it is the only way, and IS NULL needs it.
    db.query("MATCH (p:人物 {name: '张三'}) SET p.兴趣 = null");
    CHECK(db.query("MATCH (p:人物 {name: '张三'}) RETURN p.兴趣").at(0, 0).isNull());
    CHECK_EQ(db.query("MATCH (p:人物 {name: '张三'}) WHERE p.兴趣 IS NULL RETURN count(*) AS n")
                 .at(0, 0)
                 .asInt(),
             1);

    db.query("CREATE (:城市 {name: '北京'})");
    db.query("CREATE (:城市 {name: '北京'})");
    CHECK_EQ(db.query("MATCH (c:城市) RETURN c.name AS n").size(), 2u);
    CHECK_EQ(db.query("MATCH (c:城市) RETURN DISTINCT c.name AS n").size(), 1u);

    auto paged = db.query("MATCH (p:人物) RETURN p.name AS n ORDER BY n SKIP 1 LIMIT 2");
    CHECK_EQ(paged.size(), 2u);
    CHECK_EQ(asText(paged, 0, 0), std::string("李四"));  // 张 < 李 < 王 in UTF-8 byte order

    // DETACH DELETE is standard Cypher; accept it as the same thing as DELETE.
    auto detached = db.query("MATCH (p:人物 {name: '李四'}) DETACH DELETE p");
    CHECK_EQ(detached.nodes_deleted, 1);
    CHECK_EQ(detached.edges_deleted, 1);
    CHECK_EQ(db.edgeCount(), 1u);  // the 同事 edge survives
}

void testIndexHasNoDuplicates() {
    section("an updated property must not match the node twice");

    // Within one session the property index is append-only, so re-setting a
    // value pushes the owner again. Without deduplication the node matches
    // twice and every result doubles -- a wrong answer, not an error.
    gdb::Database db;
    db.query("CREATE (p:Thing {k: 'v'})");
    CHECK_EQ(db.query("MATCH (p:Thing {k: 'v'}) RETURN p.k").size(), 1u);

    db.query("MATCH (p:Thing {k: 'v'}) SET p.k = 'v'");
    CHECK_EQ(db.query("MATCH (p:Thing {k: 'v'}) RETURN p.k").size(), 1u);

    db.query("MATCH (p:Thing {k: 'v'}) SET p.k = 'v'");
    CHECK_EQ(db.query("MATCH (p:Thing {k: 'v'}) RETURN p.k").size(), 1u);
    CHECK_EQ(asInt(db.query("MATCH (p:Thing {k: 'v'}) RETURN count(*) AS n")), 1);

    db.query("MATCH (p:Thing {k: 'v'}) SET p += {k: 'v'}");
    CHECK_EQ(db.query("MATCH (p:Thing {k: 'v'}) RETURN p.k").size(), 1u);

    db.query("MATCH (p:Thing {k: 'v'}) SET p = {k: 'v'}");
    CHECK_EQ(db.query("MATCH (p:Thing {k: 'v'}) RETURN p.k").size(), 1u);

    // The same through MERGE, which is what the nightly job actually does.
    for (int i = 0; i < 3; ++i) db.query("MERGE (p:Thing {k: 'v'}) SET p.k = 'v'");
    CHECK_EQ(db.query("MATCH (p:Thing {k: 'v'}) RETURN p.k").size(), 1u);

    // And a traversal starting from the updated node must not fan out either.
    db.query("MERGE (p:Thing {k: 'v'}) MERGE (q:Other {o: 1}) MERGE (p)-[:R]->(q)");
    CHECK_EQ(db.query("MATCH (p:Thing {k: 'v'})-[:R]->(q) RETURN q.o").size(), 1u);
    CHECK_EQ(asInt(db.query("MATCH (p:Thing {k: 'v'})-[:R]->(q) RETURN count(*) AS n")), 1);
}

void testRowLimit() {
    section("a traversal blow-up is an error, not an out-of-memory");

    gdb::Database db;
    for (int i = 0; i < 40; ++i) {
        db.query("CREATE (:N {i: " + std::to_string(i) + "})");
    }
    // Fully connect the first twenty nodes: two hops is 20*19*19 rows.
    for (int i = 0; i < 20; ++i) {
        for (int j = 0; j < 20; ++j) {
            if (i == j) continue;
            db.query("MATCH (a:N {i: " + std::to_string(i) + "}), (b:N {i: " +
                     std::to_string(j) + "}) CREATE (a)-[:R]->(b)");
        }
    }
    CHECK(db.edgeCount() >= 380u);

    CHECK_EQ(db.rowLimit(), gdb::kDefaultRowLimit);
    db.setRowLimit(100);
    CHECK_EQ(db.rowLimit(), 100u);

    bool refused = false;
    try {
        db.query("MATCH (a:N)-[:R]->(b)-[:R]->(c) RETURN c LIMIT 1");
    } catch (const gdb::Error& e) {
        refused = std::string(e.what()).find("intermediate rows") != std::string::npos;
    }
    CHECK(refused);

    // The same query is fine once the ceiling is back to normal.
    db.setRowLimit(gdb::kDefaultRowLimit);
    CHECK(db.query("MATCH (a:N)-[:R]->(b)-[:R]->(c) RETURN c").size() > 100u);

    // The footprint estimate comes from the real arrays, so it must be non-zero
    // and must grow with the data.
    gdb::Database small;
    small.query("CREATE (:N {i: 1})");
    CHECK(small.estimatedBytes() > 0);
    CHECK(db.estimatedBytes() > small.estimatedBytes());
}

void testDirectApi() {
    section("direct (non-Cypher) API");
    gdb::Database db;
    const gdb::Id a = db.addNode("Item", {{"sku", gdb::Value::text("A-1")},
                                          {"price", gdb::Value::text("9.5")}});
    const gdb::Id b = db.addNode("Item", {{"sku", gdb::Value::text("B-2")}});
    const gdb::Id e = db.addEdge("NEXT", a, b, {{"rank", gdb::Value::text("1")}});

    CHECK(db.nodeExists(a));
    CHECK_EQ(db.nodeLabel(a), std::string("Item"));
    CHECK_EQ(db.nodeProp(a, "sku").asText(), std::string("A-1"));
    CHECK_EQ(db.nodeProp(a, "price").asText(), std::string("9.5"));
    CHECK_EQ(db.outEdges(a).size(), 1u);
    CHECK_EQ(db.inEdges(b).size(), 1u);
    CHECK_EQ(db.edgesBetween(a, b).size(), 1u);
    CHECK_EQ(db.edgeType(e), std::string("NEXT"));
    CHECK_EQ(db.edgeProp(e, "rank").asInt(), 1);  // asInt() is a caller convenience

    CHECK(db.setNodeProp(a, "price", gdb::Value::text("11.0")));
    CHECK_EQ(db.nodeProp(a, "price").asText(), std::string("11.0"));
    CHECK(db.removeNodeProp(a, "price"));
    CHECK(!db.nodeHasProp(a, "price"));

    // A multi-valued property is one key holding a list, which is how a
    // relationship-free set such as interests is stored.
    CHECK(db.setNodeProp(a, "兴趣", gdb::Value::list({"天文学", "编程"})));
    CHECK_EQ(db.nodeProp(a, "兴趣").type, gdb::ValueType::List);
    CHECK_EQ(db.nodeProp(a, "兴趣").items.size(), 2u);
    CHECK_EQ(db.nodeProp(a, "兴趣").items.at(0), std::string("天文学"));

    CHECK(db.removeEdge(e));
    CHECK_EQ(db.edgeCount(), 0u);
    CHECK(db.removeNode(a));
    CHECK_EQ(db.nodeCount(), 1u);

    auto hits = db.nodesWithLabel("Item");
    CHECK_EQ(hits.size(), 1u);
    CHECK_EQ(hits.at(0), b);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep output when a check crashes
    std::printf("gdb tests\n");

    try {
        gdb::Database db;
        testCreateAndMatch(db);
        testTraversal(db);
        testWhereOperators(db);
        testCounting(db);
        testOrderingAndPaging(db);
        testWriteBack(db);
        testPersistence(db);
        testRebuildIsLossless();
        testErrors();
        testUtf8();
        testMergeAndCrud();
        testIndexHasNoDuplicates();
        testRowLimit();
        testWriterConflict();
        testDirectApi();
    } catch (const std::exception& e) {
        std::printf("unexpected exception: %s\n", e.what());
        return 1;
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

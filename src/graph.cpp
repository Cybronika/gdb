// graph.cpp - value semantics, in-memory store, direct (non-Cypher) API.

#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

namespace gdb {

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Value Value::null() { return Value(); }

Value Value::text(std::string v) {
    Value x;
    x.type = ValueType::Str;
    x.s = std::move(v);
    return x;
}

Value Value::list(std::vector<std::string> v) {
    Value x;
    x.type = ValueType::List;
    x.items = std::move(v);
    return x;
}

Value Value::node(Id id) {
    Value x;
    x.type = ValueType::Node;
    x.ref = id;
    return x;
}

Value Value::rel(Id id) {
    Value x;
    x.type = ValueType::Rel;
    x.ref = id;
    return x;
}

// Caller conveniences only: the engine never parses property text itself. Text
// that is not a number is an error rather than a silent zero.
std::int64_t Value::asInt() const {
    switch (type) {
    case ValueType::Null: return 0;
    case ValueType::Str: {
        try {
            std::size_t used = 0;
            const long long parsed = std::stoll(s, &used);
            if (used != s.size()) throw std::invalid_argument("trailing text");
            return static_cast<std::int64_t>(parsed);
        } catch (...) {
            throw Error("cannot read '" + s + "' as an integer");
        }
    }
    default:
        throw Error("cannot read '" + asText() + "' as an integer");
    }
}

bool Value::asBool() const {
    switch (type) {
    case ValueType::Null: return false;
    case ValueType::List: return !items.empty();
    case ValueType::Str:
        return !s.empty() && s != "false" && s != "0" && s != "null";
    default: return true;
    }
}

std::string Value::asText() const {
    switch (type) {
    case ValueType::Str: return s;
    case ValueType::Node: return "#" + std::to_string(ref);
    case ValueType::Rel: return "@" + std::to_string(ref);
    case ValueType::List: {
        std::string out;
        for (const std::string& item : items) {
            if (!out.empty()) out += ", ";
            out += item;
        }
        return out;
    }
    default: return toString();
    }
}

std::string Value::toString() const {
    switch (type) {
    case ValueType::Null: return "null";
    case ValueType::Str: return s;
    case ValueType::List: {
        // Length-prefixed so that ["a,b"] and ["a","b"] can never collide in the
        // index key or in a DISTINCT comparison.
        std::string out = "[";
        for (const std::string& item : items) {
            out += std::to_string(item.size());
            out += ':';
            out += item;
        }
        out += "]";
        return out;
    }
    case ValueType::Node: return "#" + std::to_string(ref);
    case ValueType::Rel: return "@" + std::to_string(ref);
    }
    return "null";
}

bool Value::operator==(const Value& other) const {
    if (type != other.type) return false;
    switch (type) {
    case ValueType::Null: return true;
    case ValueType::Str: return s == other.s;
    case ValueType::List: return items == other.items;
    case ValueType::Node:
    case ValueType::Rel: return ref == other.ref;
    }
    return false;
}

// Terminal columns, not bytes and not code points: a CJK glyph occupies two
// cells. Without this every Chinese table comes out ragged.
std::size_t displayWidth(const std::string& text) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        std::uint32_t point = c;
        if (c >= 0xF0) { length = 4; point = c & 0x07u; }
        else if (c >= 0xE0) { length = 3; point = c & 0x0Fu; }
        else if (c >= 0xC0) { length = 2; point = c & 0x1Fu; }
        for (std::size_t k = 1; k < length && i + k < text.size(); ++k) {
            point = (point << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        i += length;
        const bool wide = (point >= 0x1100 && point <= 0x115F) ||
                          (point >= 0x2E80 && point <= 0xA4CF) ||
                          (point >= 0xAC00 && point <= 0xD7A3) ||
                          (point >= 0xF900 && point <= 0xFAFF) ||
                          (point >= 0xFE30 && point <= 0xFE6F) ||
                          (point >= 0xFF00 && point <= 0xFF60) ||
                          (point >= 0xFFE0 && point <= 0xFFE6) ||
                          (point >= 0x1F300 && point <= 0x1FAFF);
        width += wide ? 2 : 1;
    }
    return width;
}

std::string padTo(const std::string& text, std::size_t width) {
    const std::size_t have = displayWidth(text);
    return have >= width ? text : text + std::string(width - have, ' ');
}

std::string QueryResult::dump(const Database& db) const {
    std::vector<std::size_t> width(columns.size());
    for (std::size_t c = 0; c < columns.size(); ++c) width[c] = displayWidth(columns[c]);

    std::vector<std::vector<std::string>> cells;
    cells.reserve(rows.size());
    for (const Row& row : rows) {
        std::vector<std::string> line;
        line.reserve(columns.size());
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const Value& v = c < row.values.size() ? row.values[c] : Value::null();
            std::string text;
            if (v.type == ValueType::Node) text = db.formatNode(v.ref);
            else if (v.type == ValueType::Rel) text = db.formatEdge(v.ref);
            else text = v.toString();
            const std::size_t w = displayWidth(text);
            if (w > width[c]) width[c] = w;
            line.push_back(std::move(text));
        }
        cells.push_back(std::move(line));
    }

    std::ostringstream out;
    auto rule = [&]() {
        for (std::size_t c = 0; c < columns.size(); ++c) {
            out << '+';
            for (std::size_t k = 0; k < width[c] + 2; ++k) out << '-';
        }
        out << "+\n";
    };
    auto line = [&](const std::vector<std::string>& values) {
        for (std::size_t c = 0; c < columns.size(); ++c) {
            const std::string cell = c < values.size() ? values[c] : std::string();
            out << "| " << padTo(cell, width[c]) << ' ';
        }
        out << "|\n";
    };

    rule();
    line(columns);
    rule();
    for (const auto& l : cells) line(l);
    rule();
    return out.str();
}

const Value& QueryResult::at(std::size_t row, std::size_t column) const {
    if (row >= rows.size() || column >= rows[row].values.size())
        throw Error("result cell out of range");
    return rows[row].values[column];
}

const Value& QueryResult::at(std::size_t row, const std::string& column) const {
    for (std::size_t c = 0; c < columns.size(); ++c) {
        if (columns[c] == column) return at(row, c);
    }
    throw Error("unknown result column '" + column + "'");
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

std::uint32_t Store::intern(const std::string& s) {
    auto it = dict_ids.find(s);
    if (it != dict_ids.end()) return it->second;
    const std::uint32_t id = static_cast<std::uint32_t>(dict.size());
    if (id == kNoStr) throw Error("string dictionary overflow");
    dict.push_back(s);
    dict_ids.emplace(s, id);
    return id;
}

std::uint32_t Store::find(const std::string& s) const noexcept {
    auto it = dict_ids.find(s);
    return it == dict_ids.end() ? kNoStr : it->second;
}

const std::string& Store::text(std::uint32_t sid) const noexcept {
    static const std::string empty;
    if (sid == kNoStr || sid >= dict.size()) return empty;
    return dict[sid];
}

bool Store::nodeAlive(Id id) const noexcept {
    return id < nodes.size() && (nodes[id].flags & kFlagDeleted) == 0;
}

bool Store::edgeAlive(Id id) const noexcept {
    return id < edges.size() && (edges[id].flags & kFlagDeleted) == 0;
}

Id Store::newNode(std::uint32_t label) {
    const Id id = static_cast<Id>(nodes.size());
    if (id == kNoId) throw Error("node space exhausted");
    NodeRec rec;
    rec.label = label;
    nodes.push_back(rec);
    label_index[label].push_back(id);
    ++node_count;
    return id;
}

Id Store::newEdge(std::uint32_t type, Id src, Id dst) {
    if (!nodeAlive(src) || !nodeAlive(dst))
        throw Error("cannot create a relationship with a missing endpoint");
    const Id id = static_cast<Id>(edges.size());
    if (id == kNoId) throw Error("relationship space exhausted");

    EdgeRec rec;
    rec.type = type;
    rec.src = src;
    rec.dst = dst;
    rec.next_out = nodes[src].first_out;
    rec.next_in = nodes[dst].first_in;
    edges.push_back(rec);
    nodes[src].first_out = id;
    nodes[dst].first_in = id;
    type_index[type].push_back(id);
    ++edge_count;
    return id;
}

bool Store::killEdge(Id id) {
    if (!edgeAlive(id)) return false;
    EdgeRec& er = edges[id];

    if (er.src != kNoId && er.src < nodes.size()) {
        Id* link = &nodes[er.src].first_out;
        while (*link != kNoId && *link != id) link = &edges[*link].next_out;
        if (*link == id) *link = er.next_out;
    }
    if (er.dst != kNoId && er.dst < nodes.size()) {
        Id* link = &nodes[er.dst].first_in;
        while (*link != kNoId && *link != id) link = &edges[*link].next_in;
        if (*link == id) *link = er.next_in;
    }

    er.flags |= kFlagDeleted;
    er.next_out = kNoId;
    er.next_in = kNoId;
    --edge_count;
    return true;
}

bool Store::killNode(Id id) {
    if (!nodeAlive(id)) return false;

    // Copy the incident edge lists first: unlinking mutates the chains.
    std::vector<Id> incident;
    for (Id e = nodes[id].first_out; e != kNoId; e = edges[e].next_out) incident.push_back(e);
    for (Id e = nodes[id].first_in; e != kNoId; e = edges[e].next_in) incident.push_back(e);
    for (Id e : incident) killEdge(e);

    nodes[id].flags |= kFlagDeleted;
    nodes[id].first_out = kNoId;
    nodes[id].first_in = kNoId;
    --node_count;
    return true;
}

void propFromValue(const Value& value, PropRec& rec) {
    rec.sid = kNoStr;
    rec.first_item = 0xFFFFFFFFu;
    switch (value.type) {
    case ValueType::Str:
        rec.vtype = kVTypeStr;
        break;
    case ValueType::List:
        rec.vtype = kVTypeList;
        break;
    case ValueType::Null:
        rec.vtype = 0;
        break;
    default:
        throw Error("only text and lists of text can be stored as properties");
    }
}

Value valueFromProp(const PropRec& rec, const std::vector<std::string>& dict,
                    const std::vector<ListItem>& items) {
    if (rec.vtype == kVTypeStr) {
        return Value::text(rec.sid < dict.size() ? dict[rec.sid] : std::string());
    }
    if (rec.vtype == kVTypeList) {
        std::vector<std::string> out;
        for (std::uint32_t item = rec.first_item; item != 0xFFFFFFFFu && item < items.size();
             item = items[item].next) {
            if (items[item].sid < dict.size()) out.push_back(dict[items[item].sid]);
        }
        return Value::list(std::move(out));
    }
    return Value::null();
}

// List elements are prepended one at a time, so the chain ends up reversed and
// has to be turned back around to read in the order that was written.
std::uint32_t Store::internList(const std::vector<std::string>& values) {
    std::uint32_t head = 0xFFFFFFFFu;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        ListItem item;
        item.sid = intern(*it);
        item.next = head;
        head = static_cast<std::uint32_t>(items.size());
        items.push_back(item);
    }
    return head;
}

bool Store::setProp(Id owner, bool on_node, std::uint32_t key, const Value& value) {
    if (on_node ? !nodeAlive(owner) : !edgeAlive(owner)) return false;

    Id link = on_node ? nodes[owner].first_prop : edges[owner].first_prop;
    while (link != kNoId) {
        if (props[link].key == key) {
            propFromValue(value, props[link]);
            if (props[link].vtype == kVTypeStr) props[link].sid = intern(value.s);
            else if (props[link].vtype == kVTypeList) props[link].first_item = internList(value.items);
            if (on_node) node_prop_index[propKey(key, value)].push_back(owner);
            return true;
        }
        link = props[link].next;
    }

    PropRec rec;
    rec.key = key;
    propFromValue(value, rec);
    if (rec.vtype == kVTypeStr) rec.sid = intern(value.s);
    else if (rec.vtype == kVTypeList) rec.first_item = internList(value.items);
    rec.next = on_node ? nodes[owner].first_prop : edges[owner].first_prop;

    const Id slot = static_cast<Id>(props.size());
    props.push_back(rec);
    if (on_node) nodes[owner].first_prop = slot;
    else edges[owner].first_prop = slot;

    if (on_node) node_prop_index[propKey(key, value)].push_back(owner);
    return true;
}

bool Store::dropProp(Id owner, bool on_node, std::uint32_t key) {
    if (on_node ? !nodeAlive(owner) : !edgeAlive(owner)) return false;

    Id* link = on_node ? &nodes[owner].first_prop : &edges[owner].first_prop;
    while (*link != kNoId) {
        if (props[*link].key == key) {
            *link = props[*link].next;
            return true;
        }
        link = &props[*link].next;
    }
    return false;
}

Value Store::getProp(Id owner, bool on_node, std::uint32_t key) const {
    if (on_node ? !nodeAlive(owner) : !edgeAlive(owner)) return Value::null();
    for (Id link = on_node ? nodes[owner].first_prop : edges[owner].first_prop;
         link != kNoId; link = props[link].next) {
        if (props[link].key == key) return valueFromProp(props[link], dict, items);
    }
    return Value::null();
}

std::vector<Id> Store::nodesByLabel(std::uint32_t label) const {
    std::vector<Id> out;
    auto it = label_index.find(label);
    if (it == label_index.end()) return out;
    out.reserve(it->second.size());
    for (Id id : it->second) {
        if (nodeAlive(id)) out.push_back(id);
    }
    return out;
}

std::vector<Id> Store::nodesByProp(std::uint32_t key, const Value& value) const {
    std::vector<Id> out;
    if (key == kNoStr) return out;
    auto it = node_prop_index.find(propKey(key, value));
    if (it == node_prop_index.end()) return out;
    out.reserve(it->second.size());
    for (Id id : it->second) {
        if (nodeAlive(id)) out.push_back(id);
    }
    // The index is append-only: re-setting a property to the same value, or
    // dropping and re-adding it, pushes the owner a second time. A duplicate
    // here would surface as the same node matching twice in one pattern, which
    // is a silently wrong answer rather than an error.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Store::rebuildIndexes() {
    label_index.clear();
    type_index.clear();
    node_prop_index.clear();

    for (Id id = 0; id < nodes.size(); ++id) {
        if (nodes[id].flags & kFlagDeleted) continue;
        label_index[nodes[id].label].push_back(id);
        for (Id p = nodes[id].first_prop; p != kNoId; p = props[p].next) {
            node_prop_index[propKey(props[p].key, valueFromProp(props[p], dict, items))].push_back(id);
        }
    }
    for (Id id = 0; id < edges.size(); ++id) {
        if (edges[id].flags & kFlagDeleted) continue;
        type_index[edges[id].type].push_back(id);
    }
}

void Store::reset() {
    nodes.clear();
    edges.clear();
    props.clear();
    items.clear();
    dict.clear();
    dict_ids.clear();
    label_index.clear();
    type_index.clear();
    node_prop_index.clear();
    node_count = 0;
    edge_count = 0;
}

std::size_t Store::tombstoneCount() const noexcept {
    std::size_t reachable = 0;
    for (const NodeRec& n : nodes) {
        if (n.flags & kFlagDeleted) continue;
        for (Id p = n.first_prop; p != kNoId; p = props[p].next) ++reachable;
    }
    for (const EdgeRec& e : edges) {
        if (e.flags & kFlagDeleted) continue;
        for (Id p = e.first_prop; p != kNoId; p = props[p].next) ++reachable;
    }
    return (nodes.size() - node_count) + (edges.size() - edge_count) +
           (props.size() - reachable);
}

std::uint64_t hashValue(const Value& value) {
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const unsigned char* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
    };
    switch (value.type) {
    case ValueType::Str:
        mix(reinterpret_cast<const unsigned char*>(value.s.data()), value.s.size());
        break;
    case ValueType::List:
        // Length-prefixed, so ["a,b"] and ["a","b"] hash differently.
        for (const std::string& item : value.items) {
            const std::uint64_t size = item.size();
            mix(reinterpret_cast<const unsigned char*>(&size), sizeof(size));
            mix(reinterpret_cast<const unsigned char*>(item.data()), item.size());
        }
        break;
    default:
        break;
    }
    // FNV-1a leaves the low bits poorly mixed for similar inputs ("1700000000",
    // "1700000001", ...), and the index buckets on exactly those bits. A final
    // avalanche is four instructions and keeps sequential keys from clustering.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    return hash;
}

PropKey propKey(std::uint32_t key, const Value& value) {
    PropKey out;
    out.key = key;
    out.hash = hashValue(value);
    return out;
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::Database() : store_(std::make_unique<Store>()) {}
Database::~Database() = default;
Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

Store& Database::store() noexcept { return *store_; }
const Store& Database::store() const noexcept { return *store_; }

Id Database::addNode(const std::string& label, const Properties& props) {
    const Id id = store_->newNode(store_->intern(label));
    for (const Prop& p : props) {
        if (!p.value.isNull()) store_->setProp(id, true, store_->intern(p.key), p.value);
    }
    return id;
}

Id Database::addEdge(const std::string& type, Id src, Id dst, const Properties& props) {
    const Id id = store_->newEdge(store_->intern(type), src, dst);
    for (const Prop& p : props) {
        if (!p.value.isNull()) store_->setProp(id, false, store_->intern(p.key), p.value);
    }
    return id;
}

bool Database::removeNode(Id id) { return store_->killNode(id); }
bool Database::removeEdge(Id id) { return store_->killEdge(id); }

bool Database::setNodeProp(Id id, const std::string& key, const Value& value) {
    if (value.isNull()) return store_->dropProp(id, true, store_->intern(key));
    return store_->setProp(id, true, store_->intern(key), value);
}

bool Database::setEdgeProp(Id id, const std::string& key, const Value& value) {
    if (value.isNull()) return store_->dropProp(id, false, store_->intern(key));
    return store_->setProp(id, false, store_->intern(key), value);
}

bool Database::removeNodeProp(Id id, const std::string& key) {
    return store_->dropProp(id, true, store_->intern(key));
}

bool Database::removeEdgeProp(Id id, const std::string& key) {
    return store_->dropProp(id, false, store_->intern(key));
}

bool Database::nodeExists(Id id) const noexcept { return store_->nodeAlive(id); }
bool Database::edgeExists(Id id) const noexcept { return store_->edgeAlive(id); }
std::size_t Database::nodeCount() const noexcept { return store_->node_count; }
std::size_t Database::edgeCount() const noexcept { return store_->edge_count; }
std::size_t Database::tombstoneCount() const noexcept { return store_->tombstoneCount(); }

void Database::setRowLimit(std::size_t rows) noexcept { row_limit_ = rows; }
std::size_t Database::rowLimit() const noexcept { return row_limit_; }

std::size_t Database::estimatedBytes() const noexcept {
    const Store& s = *store_;
    std::size_t bytes = s.nodes.size() * sizeof(NodeRec) + s.edges.size() * sizeof(EdgeRec) +
                        s.props.size() * sizeof(PropRec) + s.items.size() * sizeof(ListItem);
    // Each dictionary entry is a std::string object plus, for anything past the
    // small-string buffer, a heap block.
    for (const std::string& text : s.dict) {
        bytes += sizeof(std::string) + text.size() + 24;
    }
    bytes += s.dict_ids.size() * 64;
    for (const auto& kv : s.label_index) bytes += kv.second.size() * sizeof(Id) + 48;
    for (const auto& kv : s.type_index) bytes += kv.second.size() * sizeof(Id) + 48;
    for (const auto& kv : s.node_prop_index) bytes += kv.second.size() * sizeof(Id) + 96;
    return bytes;
}

std::string Database::nodeLabel(Id id) const {
    if (!store_->nodeAlive(id)) return std::string();
    return store_->text(store_->nodes[id].label);
}

std::string Database::edgeType(Id id) const {
    if (!store_->edgeAlive(id)) return std::string();
    return store_->text(store_->edges[id].type);
}

Id Database::edgeSource(Id id) const {
    return store_->edgeAlive(id) ? store_->edges[id].src : kNoId;
}

Id Database::edgeTarget(Id id) const {
    return store_->edgeAlive(id) ? store_->edges[id].dst : kNoId;
}

Value Database::nodeProp(Id id, const std::string& key) const {
    return store_->getProp(id, true, store_->find(key));
}

Value Database::edgeProp(Id id, const std::string& key) const {
    return store_->getProp(id, false, store_->find(key));
}

Properties Database::nodeProps(Id id) const {
    Properties out;
    if (!store_->nodeAlive(id)) return out;
    for (Id p = store_->nodes[id].first_prop; p != kNoId; p = store_->props[p].next) {
        out.push_back(Prop{store_->text(store_->props[p].key),
                           valueFromProp(store_->props[p], store_->dict, store_->items)});
    }
    return out;
}

Properties Database::edgeProps(Id id) const {
    Properties out;
    if (!store_->edgeAlive(id)) return out;
    for (Id p = store_->edges[id].first_prop; p != kNoId; p = store_->props[p].next) {
        out.push_back(Prop{store_->text(store_->props[p].key),
                           valueFromProp(store_->props[p], store_->dict, store_->items)});
    }
    return out;
}

bool Database::nodeHasProp(Id id, const std::string& key) const {
    if (!store_->nodeAlive(id)) return false;
    const std::uint32_t k = store_->find(key);
    if (k == kNoStr) return false;
    for (Id p = store_->nodes[id].first_prop; p != kNoId; p = store_->props[p].next) {
        if (store_->props[p].key == k) return true;
    }
    return false;
}

std::vector<Id> Database::allNodes() const {
    std::vector<Id> out;
    out.reserve(store_->node_count);
    for (Id id = 0; id < store_->nodes.size(); ++id) {
        if (store_->nodeAlive(id)) out.push_back(id);
    }
    return out;
}

std::vector<Id> Database::allEdges() const {
    std::vector<Id> out;
    out.reserve(store_->edge_count);
    for (Id id = 0; id < store_->edges.size(); ++id) {
        if (store_->edgeAlive(id)) out.push_back(id);
    }
    return out;
}

std::vector<Id> Database::nodesWithLabel(const std::string& label) const {
    const std::uint32_t sid = store_->find(label);
    if (sid == kNoStr) return {};
    return store_->nodesByLabel(sid);
}

std::vector<EdgeRef> Database::outEdges(Id id) const {
    std::vector<EdgeRef> out;
    if (!store_->nodeAlive(id)) return out;
    for (Id e = store_->nodes[id].first_out; e != kNoId; e = store_->edges[e].next_out) {
        out.push_back(EdgeRef{e, store_->text(store_->edges[e].type),
                              store_->edges[e].src, store_->edges[e].dst});
    }
    return out;
}

std::vector<EdgeRef> Database::inEdges(Id id) const {
    std::vector<EdgeRef> out;
    if (!store_->nodeAlive(id)) return out;
    for (Id e = store_->nodes[id].first_in; e != kNoId; e = store_->edges[e].next_in) {
        out.push_back(EdgeRef{e, store_->text(store_->edges[e].type),
                              store_->edges[e].src, store_->edges[e].dst});
    }
    return out;
}

std::vector<EdgeRef> Database::edgesBetween(Id src, Id dst) const {
    std::vector<EdgeRef> out;
    if (!store_->nodeAlive(src) || !store_->nodeAlive(dst)) return out;
    for (Id e = store_->nodes[src].first_out; e != kNoId; e = store_->edges[e].next_out) {
        if (store_->edges[e].dst == dst) {
            out.push_back(EdgeRef{e, store_->text(store_->edges[e].type), src, dst});
        }
    }
    return out;
}

std::string Database::formatNode(Id id) const {
    if (!store_->nodeAlive(id)) return "#" + std::to_string(id) + " (deleted)";
    std::ostringstream out;
    out << "(" << id << ":" << store_->text(store_->nodes[id].label) << " {";
    bool first = true;
    for (Prop& p : nodeProps(id)) {
        if (!first) out << ", ";
        first = false;
        out << p.key << ": " << formatValue(p.value);
    }
    out << "})";
    return out.str();
}

std::string Database::formatEdge(Id id) const {
    if (!store_->edgeAlive(id)) return "@" + std::to_string(id) + " (deleted)";
    std::ostringstream out;
    out << "[" << id << ":" << store_->text(store_->edges[id].type) << " {";
    bool first = true;
    for (Prop& p : edgeProps(id)) {
        if (!first) out << ", ";
        first = false;
        out << p.key << ": " << formatValue(p.value);
    }
    out << "}]";
    return out.str();
}

std::string Database::formatValue(const Value& value) const {
    switch (value.type) {
    case ValueType::Node: return formatNode(value.ref);
    case ValueType::Rel: return formatEdge(value.ref);
    case ValueType::List: {
        std::string out = "[";
        for (std::size_t i = 0; i < value.items.size(); ++i) {
            if (i) out += ", ";
            out += formatValue(Value::text(value.items[i]));
        }
        out += "]";
        return out;
    }
    case ValueType::Str: {
        std::string escaped = "'";
        for (char c : value.s) {
            if (c == '\'') escaped += "\\'";
            else if (c == '\\') escaped += "\\\\";
            else escaped.push_back(c);
        }
        escaped += "'";
        return escaped;
    }
    default: return value.toString();
    }
}

}  // namespace gdb

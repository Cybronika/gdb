// internal.h - storage layout. Not part of the public API.
//
// Layout notes
// ------------
// Everything is index based. An Id is simply a slot in a vector, and deleted
// slots are tombstoned rather than removed, so ids stay valid for the whole
// life of a Store. All strings (labels, relationship types, property keys and
// string values) live in one intern table, so comparisons during traversal are
// integer compares.
//
// Properties are per-owner singly linked lists, mirroring the adjacency lists.
// Appending is O(1); nothing ever needs a free-list.

#pragma once

#include "gdb/gdb.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdb {

inline constexpr std::uint32_t kFlagDeleted = 1u;

struct NodeRec {
    std::uint32_t label = kNoStr;
    std::uint32_t flags = 0;
    Id first_prop = kNoId;
    Id first_out = kNoId;
    Id first_in = kNoId;
};

struct EdgeRec {
    std::uint32_t type = kNoStr;
    std::uint32_t flags = 0;
    Id src = kNoId;
    Id dst = kNoId;
    Id next_out = kNoId;
    Id next_in = kNoId;
    Id first_prop = kNoId;
};

enum : std::uint8_t { kVTypeStr = 1, kVTypeList = 2 };

struct PropRec {
    std::uint32_t key = kNoStr;
    std::uint8_t vtype = 0;
    std::uint8_t pad[3] = {0, 0, 0};
    Id next = kNoId;             // next property of the same owner
    std::uint32_t sid = kNoStr;  // the text, for kVTypeStr
    std::uint32_t first_item = 0xFFFFFFFFu;  // first ListItem, for kVTypeList
};

// List elements live in their own chain. Nothing references a ListItem index
// except the PropRec that owns it, so they can be renumbered freely.
struct ListItem {
    std::uint32_t sid = kNoStr;
    std::uint32_t next = 0xFFFFFFFFu;
};

// Canonical property index key: the interned property name plus a hash of the
// value. Deliberately a small POD rather than a "key\x1fvalue" string -- the
// index is the single largest memory consumer and the reason load time is
// linear over the graph, so keeping it allocation free matters more here than
// anywhere else. Collisions only add candidates, never wrong answers: every
// candidate is revalidated against the real property value before use.
struct PropKey {
    std::uint32_t key = kNoStr;
    std::uint32_t reserved = 0;
    std::uint64_t hash = 0;

    bool operator==(const PropKey& other) const {
        return key == other.key && hash == other.hash;
    }
};

struct PropKeyHash {
    std::size_t operator()(const PropKey& k) const noexcept {
        return static_cast<std::size_t>(
            k.hash ^ (static_cast<std::uint64_t>(k.key) * 0x9E3779B97F4A7C15ull));
    }
};

// Int and Real hash through double so that 30 and 30.0 share a bucket, matching
// Value::operator==, which also treats them as equal.
std::uint64_t hashValue(const Value& value);
PropKey propKey(std::uint32_t key, const Value& value);

class Store {
public:
    std::vector<NodeRec> nodes;
    std::vector<EdgeRec> edges;
    std::vector<PropRec> props;
    std::vector<ListItem> items;

    std::vector<std::string> dict;
    std::unordered_map<std::string, std::uint32_t> dict_ids;

    // label id -> node ids, relationship type id -> edge ids
    std::unordered_map<std::uint32_t, std::vector<Id>> label_index;
    std::unordered_map<std::uint32_t, std::vector<Id>> type_index;

    // Nodes only: relationships are always reached by traversal from a node, so
    // indexing their properties would be dead weight.
    std::unordered_map<PropKey, std::vector<Id>, PropKeyHash> node_prop_index;

    std::size_t node_count = 0;
    std::size_t edge_count = 0;

    // Generation of the file this state was loaded from, or last saved as.
    // Zero means "not associated with a file yet".
    std::uint64_t generation = 0;

    // --- intern table ------------------------------------------------------
    std::uint32_t intern(const std::string& s);
    std::uint32_t find(const std::string& s) const noexcept;  // kNoStr when absent
    const std::string& text(std::uint32_t sid) const noexcept;

    // --- liveness ----------------------------------------------------------
    bool nodeAlive(Id id) const noexcept;
    bool edgeAlive(Id id) const noexcept;

    // --- mutation ----------------------------------------------------------
    Id newNode(std::uint32_t label);
    Id newEdge(std::uint32_t type, Id src, Id dst);
    bool killNode(Id id);   // also kills every incident edge
    bool killEdge(Id id);

    bool setProp(Id owner, bool on_node, std::uint32_t key, const Value& value);
    bool dropProp(Id owner, bool on_node, std::uint32_t key);
    Value getProp(Id owner, bool on_node, std::uint32_t key) const;
    std::uint32_t internList(const std::vector<std::string>& values);

    // --- lookup ------------------------------------------------------------
    std::vector<Id> nodesByLabel(std::uint32_t label) const;
    std::vector<Id> nodesByProp(std::uint32_t key, const Value& value) const;

    // --- maintenance -------------------------------------------------------
    void rebuildIndexes();
    void reset();
    std::size_t tombstoneCount() const noexcept;
};

// Every property value is text or a list of text; see Value in gdb.h.
void propFromValue(const Value& value, PropRec& rec);
Value valueFromProp(const PropRec& rec, const std::vector<std::string>& dict,
                    const std::vector<ListItem>& items);

}  // namespace gdb

// storage.cpp - the .gdb snapshot format.
//
// A .gdb file is a dump of the in-memory arrays: header, intern table, node
// records, edge records, property records. There is no page manager, no free
// list and no write-ahead log. Saving rewrites the whole file and swaps it in
// with a rename, which is what makes "no daemon, no corruption" cheap.
//
// Byte order is little-endian (x86-64 / ARM64). The record structs are written
// with memcpy, so the static_asserts below are the format's contract: a layout
// change fails to compile instead of silently corrupting a file.

#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace gdb {
namespace {

constexpr char kMagic[4] = {'G', 'D', 'B', '1'};
constexpr std::uint32_t kVersion = 3;

// generation is bumped on every save. It is how a writer notices that someone
// else (the nightly job, another process) has changed the file since it was
// loaded, instead of silently overwriting a night of work.
struct FileHeader {
    char magic[4];
    std::uint32_t version;
    std::uint32_t flags;
    std::uint32_t reserved0;
    std::uint64_t node_count;
    std::uint64_t edge_count;
    std::uint64_t prop_count;
    std::uint64_t item_count;
    std::uint64_t dict_count;
    std::uint64_t payload_bytes;
    std::uint64_t checksum;
    std::uint64_t generation;
};

static_assert(sizeof(FileHeader) == 80, "gdb header layout changed");
static_assert(sizeof(NodeRec) == 32, "gdb node record layout changed");
static_assert(sizeof(EdgeRec) == 48, "gdb edge record layout changed");
static_assert(sizeof(PropRec) == 24, "gdb property record layout changed");
static_assert(sizeof(ListItem) == 8, "gdb list item layout changed");

// Reads only the header. Returns false when the file is absent or unreadable as
// a gdb file, which the caller treats as "generation 0".
bool readGeneration(const std::string& path, std::uint64_t& out) {
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&file, std::filesystem::path(path).wstring().c_str(), L"rb") != 0) return false;
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) return false;
    FileHeader header{};
    const std::size_t got = std::fread(&header, 1, sizeof(header), file);
    std::fclose(file);
    if (got != sizeof(header)) return false;
    if (std::memcmp(header.magic, kMagic, 4) != 0) return false;
    out = header.generation;
    return true;
}

std::FILE* openForWrite(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.wstring().c_str(), L"wb") != 0) return nullptr;
    return file;
#else
    return std::fopen(path.string().c_str(), "wb");
#endif
}

void flushToDisk(std::FILE* file) {
    if (std::fflush(file) != 0) return;
#if defined(_WIN32)
    _commit(_fileno(file));
#else
    fsync(fileno(file));
#endif
}

std::uint64_t fnv1a(const char* data, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename T>
void appendRaw(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void appendArray(std::string& out, const std::vector<T>& values) {
    if (!values.empty()) {
        out.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
    }
}

class Reader {
public:
    Reader(const char* data, std::size_t size, const std::string& path)
        : data_(data), size_(size), path_(path) {}

    template <typename T>
    T take() {
        if (offset_ + sizeof(T) > size_) fail("truncated file");
        T value;
        std::memcpy(&value, data_ + offset_, sizeof(T));
        offset_ += sizeof(T);
        return value;
    }

    std::string takeString() {
        const std::uint32_t len = take<std::uint32_t>();
        if (offset_ + len > size_) fail("truncated string table");
        std::string out(data_ + offset_, len);
        offset_ += len;
        return out;
    }

    template <typename T>
    std::vector<T> takeArray(std::size_t count) {
        if (count > (size_ - offset_) / sizeof(T)) fail("truncated section");
        std::vector<T> out(count);
        if (count) std::memcpy(out.data(), data_ + offset_, count * sizeof(T));
        offset_ += count * sizeof(T);
        return out;
    }

    [[noreturn]] void fail(const std::string& why) const {
        throw Error("cannot read '" + path_ + "': " + why);
    }

private:
    const char* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
    std::string path_;
};

}  // namespace

void Database::save(const std::string& path, bool force) {
    const Store& s = *store_;

    // Guard against lost updates. Two writers exist in practice -- a nightly
    // batch job and an interactive session -- and both work by loading the whole
    // graph, editing, and writing it back. Without this check the later writer
    // silently discards the earlier one's work.
    std::uint64_t onDisk = 0;
    const bool exists = readGeneration(path, onDisk);
    if (!force && exists && onDisk != s.generation) {
        throw Error("refusing to overwrite '" + path + "': it was changed by another writer " +
                    "(on disk generation " + std::to_string(onDisk) + ", this database loaded " +
                    std::to_string(s.generation) + "). Reload the file and re-apply your change, " +
                    "or pass force to overwrite.");
    }
    if (!force && !exists && s.generation != 0) {
        throw Error("refusing to write '" + path + "': this database was loaded from it, but the " +
                    "file is gone. Pass force to write it as a new file.");
    }
    const std::uint64_t next = std::max(onDisk, s.generation) + 1;

    std::string payload;
    payload.reserve(s.nodes.size() * sizeof(NodeRec) + s.edges.size() * sizeof(EdgeRec) +
                    s.props.size() * sizeof(PropRec) + s.items.size() * sizeof(ListItem) +
                    s.dict.size() * 16 + 1024);

    // Strings own their length prefix so the table is self-describing.
    for (const std::string& text : s.dict) {
        if (text.size() > 0xFFFFFFFFull) throw Error("string too long to persist");
        appendRaw<std::uint32_t>(payload, static_cast<std::uint32_t>(text.size()));
        payload.append(text);
    }
    appendArray(payload, s.nodes);
    appendArray(payload, s.edges);
    appendArray(payload, s.props);
    appendArray(payload, s.items);

    FileHeader header{};
    std::memcpy(header.magic, kMagic, 4);
    header.version = kVersion;
    header.flags = 0;
    header.reserved0 = 0;
    header.node_count = s.nodes.size();
    header.edge_count = s.edges.size();
    header.prop_count = s.props.size();
    header.item_count = s.items.size();
    header.dict_count = s.dict.size();
    header.payload_bytes = payload.size();
    header.checksum = fnv1a(payload.data(), payload.size());
    header.generation = next;

    const std::filesystem::path target(path);
    std::filesystem::path temp = target;
    temp += ".tmp";

    std::FILE* file = openForWrite(temp);
    if (!file) throw Error("cannot open '" + temp.string() + "' for writing");
    const bool wrote = std::fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
                       (payload.empty() ||
                        std::fwrite(payload.data(), 1, payload.size(), file) == payload.size());
    // Get the bytes onto the platter before the rename makes them reachable.
    flushToDisk(file);
    std::fclose(file);
    if (!wrote) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw Error("failed while writing '" + temp.string() + "'");
    }

    // rename replaces the target atomically, so there is never a moment where
    // the database file does not exist. Removing the target first -- which this
    // used to do -- opens exactly that window.
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw Error("cannot replace '" + path + "': " + ec.message());
    }
    store_->generation = next;
}

void Database::load(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        store_->reset();
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open '" + path + "' for reading");
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (blob.size() < sizeof(FileHeader)) throw Error("cannot read '" + path + "': too short");

    FileHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    if (std::memcmp(header.magic, kMagic, 4) != 0)
        throw Error("'" + path + "' is not a gdb file (bad magic)");
    if (header.version != kVersion)
        throw Error("'" + path + "' was written by gdb format version " +
                    std::to_string(header.version) + ", this build reads version " +
                    std::to_string(kVersion));
    if (header.payload_bytes != blob.size() - sizeof(FileHeader))
        throw Error("cannot read '" + path + "': payload size mismatch (truncated file)");

    const char* payload = blob.data() + sizeof(FileHeader);
    if (fnv1a(payload, static_cast<std::size_t>(header.payload_bytes)) != header.checksum)
        throw Error("cannot read '" + path + "': checksum mismatch (file is corrupted)");

    Reader reader(payload, static_cast<std::size_t>(header.payload_bytes), path);

    Store fresh;
    fresh.dict.resize(header.dict_count);
    for (std::uint64_t i = 0; i < header.dict_count; ++i) fresh.dict[i] = reader.takeString();
    for (std::uint32_t i = 0; i < fresh.dict.size(); ++i) fresh.dict_ids.emplace(fresh.dict[i], i);

    fresh.nodes = reader.takeArray<NodeRec>(header.node_count);
    fresh.edges = reader.takeArray<EdgeRec>(header.edge_count);
    fresh.props = reader.takeArray<PropRec>(header.prop_count);
    fresh.items = reader.takeArray<ListItem>(header.item_count);

    // An edge chain entry must be in range or traversal would read wild memory.
    for (const NodeRec& n : fresh.nodes) {
        if (n.first_prop >= fresh.props.size() && n.first_prop != kNoId) reader.fail("node property link out of range");
        if (n.first_out >= fresh.edges.size() && n.first_out != kNoId) reader.fail("node out-edge link out of range");
        if (n.first_in >= fresh.edges.size() && n.first_in != kNoId) reader.fail("node in-edge link out of range");
    }
    for (const EdgeRec& e : fresh.edges) {
        if (e.first_prop >= fresh.props.size() && e.first_prop != kNoId) reader.fail("edge property link out of range");
    }
    for (const PropRec& p : fresh.props) {
        if (p.key >= fresh.dict.size()) reader.fail("property key out of range");
        if (p.vtype == kVTypeStr && p.sid >= fresh.dict.size())
            reader.fail("property string out of range");
        if (p.vtype == kVTypeList && p.first_item != 0xFFFFFFFFu &&
            p.first_item >= fresh.items.size())
            reader.fail("property list link out of range");
        if (p.next >= fresh.props.size() && p.next != kNoId) reader.fail("property link out of range");
    }
    for (const ListItem& item : fresh.items) {
        if (item.sid >= fresh.dict.size()) reader.fail("list element out of range");
        if (item.next != 0xFFFFFFFFu && item.next >= fresh.items.size())
            reader.fail("list link out of range");
    }

    fresh.node_count = 0;
    for (const NodeRec& n : fresh.nodes) {
        if ((n.flags & kFlagDeleted) == 0) ++fresh.node_count;
    }
    fresh.edge_count = 0;
    for (const EdgeRec& e : fresh.edges) {
        if ((e.flags & kFlagDeleted) == 0) ++fresh.edge_count;
    }

    fresh.rebuildIndexes();
    fresh.generation = header.generation;
    *store_ = std::move(fresh);
}

Database Database::open(const std::string& path) {
    Database db;
    db.load(path);
    return db;
}

std::uint64_t Database::generation() const noexcept { return store_->generation; }

}  // namespace gdb

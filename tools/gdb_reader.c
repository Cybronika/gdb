/* gdb_reader.c - the escape hatch.
 *
 * Plain C99. Includes only the C standard library, and nothing from this
 * project: no gdb.h, no C++ runtime, no build system beyond a C compiler.
 *
 * This exists because the promise that matters over ten years is not speed, it
 * is that the data can still be read out after the library that wrote it is
 * gone. If that ever happens, this file is the whole recovery procedure:
 *
 *     cc -O2 -o gdb_reader gdb_reader.c
 *     ./gdb_reader memory.gdb
 *
 * The .gdb format is deliberately simple to re-implement: a fixed-size header,
 * a table of length-prefixed strings, then four arrays of fixed-size records.
 * Every index in those records is bounds-checked here rather than trusted,
 * because a recovery tool that crashes on a damaged file is not a recovery tool.
 *
 * Little-endian hosts only (x86-64, ARM64). The format has no byte swapping.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_STR 0xFFFFFFFFu
#define NO_ID (~(uint64_t)0)
#define NO_ITEM 0xFFFFFFFFu

#define FLAG_DELETED 1u

#define V_TYPE_STR 1u
#define V_TYPE_LIST 2u

#pragma pack(push, 1)
typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t node_count;
    uint64_t edge_count;
    uint64_t prop_count;
    uint64_t item_count;
    uint64_t dict_count;
    uint64_t payload_bytes;
    uint64_t checksum;
    uint64_t generation;
} Header;

typedef struct {
    uint32_t label;
    uint32_t flags;
    uint64_t first_prop;
    uint64_t first_out;
    uint64_t first_in;
} NodeRec;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t src;
    uint64_t dst;
    uint64_t next_out;
    uint64_t next_in;
    uint64_t first_prop;
} EdgeRec;

typedef struct {
    uint32_t key;
    uint8_t vtype;
    uint8_t pad[3];
    uint64_t next;
    uint32_t sid;
    uint32_t first_item;
} PropRec;

typedef struct {
    uint32_t sid;
    uint32_t next;
} ListItem;
#pragma pack(pop)

static void die(const char *what, const char *path) {
    fprintf(stderr, "gdb_reader: %s: %s\n", path, what);
    exit(1);
}

static void *read_all(FILE *f, size_t bytes) {
    void *block = malloc(bytes ? bytes : 1);
    if (!block) die("out of memory", "-");
    if (bytes && fread(block, 1, bytes, f) != bytes) die("file is truncated", "-");
    return block;
}

static char *dict_text(char **dict, uint32_t sid) {
    return (sid == NO_STR) ? (char *)"" : dict[sid];
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: gdb_reader <file.gdb>\n");
        return 1;
    }
    const char *path = argv[1];

    /* The reader refuses to run if its own structs do not match the format, so a
       compiler that pads differently fails loudly instead of printing garbage. */
    if (sizeof(Header) != 80 || sizeof(NodeRec) != 32 || sizeof(EdgeRec) != 48 ||
        sizeof(PropRec) != 24 || sizeof(ListItem) != 8) {
        die("this compiler lays out structs differently from the format", path);
    }

    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open", path);

    Header h;
    if (fread(&h, 1, sizeof(h), f) != sizeof(h)) die("too short to be a gdb file", path);
    if (memcmp(h.magic, "GDB1", 4) != 0) die("not a gdb file (bad magic)", path);
    if (h.version != 3) {
        fprintf(stderr, "gdb_reader: %s: format version %u, this reader knows version 3\n", path,
                h.version);
        return 1;
    }
    long payload_start = ftell(f);
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, payload_start, SEEK_SET);
    if ((uint64_t)(file_size - payload_start) != h.payload_bytes) die("payload size mismatch", path);

    /* --- string table ---------------------------------------------------- */
    char **dict = calloc(h.dict_count ? h.dict_count : 1, sizeof(char *));
    if (!dict) die("out of memory", path);
    uint64_t i;
    for (i = 0; i < h.dict_count; ++i) {
        uint32_t len = 0;
        if (fread(&len, 1, 4, f) != 4) die("truncated string table", path);
        char *text = malloc((size_t)len + 1);
        if (!text) die("out of memory", path);
        if (len && fread(text, 1, len, f) != len) die("truncated string table", path);
        text[len] = '\0';
        dict[i] = text;
    }

    /* --- record arrays --------------------------------------------------- */
    NodeRec *nodes = read_all(f, (size_t)h.node_count * sizeof(NodeRec));
    EdgeRec *edges = read_all(f, (size_t)h.edge_count * sizeof(EdgeRec));
    PropRec *props = read_all(f, (size_t)h.prop_count * sizeof(PropRec));
    ListItem *items = read_all(f, (size_t)h.item_count * sizeof(ListItem));
    fclose(f);

    printf("%s\n", path);
    printf("  generation %llu\n", (unsigned long long)h.generation);
    printf("  records: %llu nodes, %llu edges, %llu properties, %llu list items, %llu strings\n",
           (unsigned long long)h.node_count, (unsigned long long)h.edge_count,
           (unsigned long long)h.prop_count, (unsigned long long)h.item_count,
           (unsigned long long)h.dict_count);

    uint64_t live_nodes = 0, live_edges = 0;
    for (i = 0; i < h.node_count; ++i) {
        if (!(nodes[i].flags & FLAG_DELETED)) ++live_nodes;
    }
    for (i = 0; i < h.edge_count; ++i) {
        if (!(edges[i].flags & FLAG_DELETED)) ++live_edges;
    }
    printf("  live: %llu nodes, %llu edges\n", (unsigned long long)live_nodes,
           (unsigned long long)live_edges);

    /* --- properties ------------------------------------------------------ */
    for (i = 0; i < h.node_count; ++i) {
        if (nodes[i].flags & FLAG_DELETED) continue;
        printf("(%llu:%s", (unsigned long long)i, dict_text(dict, nodes[i].label));
        uint64_t p = nodes[i].first_prop;
        while (p != NO_ID && p < h.prop_count) {
            printf(" %s: ", dict_text(dict, props[p].key));
            if (props[p].vtype == V_TYPE_STR) {
                printf("'%s'", dict_text(dict, props[p].sid));
            } else if (props[p].vtype == V_TYPE_LIST) {
                printf("[");
                uint32_t it = props[p].first_item;
                int first = 1;
                while (it != NO_ITEM && it < h.item_count) {
                    if (!first) printf(", ");
                    first = 0;
                    printf("'%s'", dict_text(dict, items[it].sid));
                    it = items[it].next;
                }
                printf("]");
            } else {
                printf("<unknown type %u>", props[p].vtype);
            }
            p = props[p].next;
        }
        printf(")\n");
    }

    for (i = 0; i < h.edge_count; ++i) {
        if (edges[i].flags & FLAG_DELETED) continue;
        if (edges[i].src >= h.node_count || edges[i].dst >= h.node_count) {
            printf("[%llu:%s] <endpoint out of range, skipping>\n", (unsigned long long)i,
                   dict_text(dict, edges[i].type));
            continue;
        }
        printf("[%llu:%s] %llu -> %llu", (unsigned long long)i, dict_text(dict, edges[i].type),
               (unsigned long long)edges[i].src, (unsigned long long)edges[i].dst);
        uint64_t p = edges[i].first_prop;
        while (p != NO_ID && p < h.prop_count) {
            printf(" %s: ", dict_text(dict, props[p].key));
            if (props[p].vtype == V_TYPE_STR) printf("'%s'", dict_text(dict, props[p].sid));
            else printf("[list]");
            p = props[p].next;
        }
        printf("\n");
    }

    for (i = 0; i < h.dict_count; ++i) free(dict[i]);
    free(dict);
    free(nodes);
    free(edges);
    free(props);
    free(items);
    return 0;
}

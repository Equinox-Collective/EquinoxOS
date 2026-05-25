#include "gpt.h"
#include "../hal/hal.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"
#include "../mem/memory.h"

extern void term_print(const char *str);

/* Helper: is a 16-byte GUID all zero? */
static bool guid_is_zero(const uint8_t *g) {
    for (int i = 0; i < 16; ++i)
        if (g[i]) return false;
    return true;
}

/* UTF-16LE -> ASCII (lossy: non-ASCII becomes '?', NUL terminates). */
static void utf16_to_ascii(const uint16_t *in, int max_in, char *out, int out_sz) {
    int j = 0;
    for (int i = 0; i < max_in && j < out_sz - 1; ++i) {
        uint16_t c = in[i];
        if (c == 0) break;
        out[j++] = (c < 0x80) ? (char)c : '?';
    }
    out[j] = '\0';
}

int gpt_parse(int block_id, gpt_partition_t *out, int max_out) {
    hal_block_ops_t *blk = hal_block(block_id);
    if (!blk || !blk->read) return -1;

    /* 1. Sector 0: protective MBR — sanity check (type 0xEE expected). */
    uint8_t mbr[512];
    if (blk->read(blk, 0, 1, mbr) != 1) return -1;

    /* MBR partition table at offset 446, each entry 16 bytes. */
    bool found_protective = false;
    for (int i = 0; i < 4; ++i) {
        uint8_t type = mbr[446 + i * 16 + 4];
        if (type == 0xEE) { found_protective = true; break; }
    }
    if (!found_protective) {
        term_print("[GPT] No protective MBR (type 0xEE) — not a GPT disk.\n");
        return -1;
    }

    /* 2. Sector 1: primary GPT header. */
    uint8_t hdr_buf[512];
    if (blk->read(blk, 1, 1, hdr_buf) != 1) return -1;

    gpt_header_t *hdr = (gpt_header_t *)hdr_buf;
    if (hdr->signature != GPT_SIGNATURE) {
        term_print("[GPT] Bad signature.\n");
        return -1;
    }
    if (hdr->revision < GPT_REVISION_1_0) {
        term_print("[GPT] Unsupported revision.\n");
        return -1;
    }
    if (hdr->partition_entry_size < sizeof(gpt_entry_raw_t) ||
        hdr->num_partition_entries == 0 ||
        hdr->num_partition_entries > GPT_MAX_PARTITIONS) {
        term_print("[GPT] Suspicious entry table geometry.\n");
        return -1;
    }

    /* 3. Read the entry array. */
    uint32_t entry_size  = hdr->partition_entry_size;
    uint32_t num_entries = hdr->num_partition_entries;
    uint64_t entries_lba = hdr->partition_entries_lba;
    uint32_t total_bytes = entry_size * num_entries;
    uint32_t sectors     = (total_bytes + blk->sector_size - 1) / blk->sector_size;

    uint8_t *table = (uint8_t *)kmalloc(sectors * blk->sector_size);
    if (!table) return -1;

    if (blk->read(blk, entries_lba, sectors, table) != (int)sectors) {
        kfree(table);
        return -1;
    }

    /* 4. Walk entries, collect used ones. */
    int found = 0;
    for (uint32_t i = 0; i < num_entries && found < max_out; ++i) {
        gpt_entry_raw_t *e = (gpt_entry_raw_t *)(table + i * entry_size);
        if (guid_is_zero(e->type_guid)) continue;  /* unused slot */
        if (e->first_lba == 0 || e->last_lba < e->first_lba) continue;

        gpt_partition_t *p = &out[found++];
        p->index        = (int)i;
        p->first_lba    = e->first_lba;
        p->last_lba     = e->last_lba;
        p->size_sectors = e->last_lba - e->first_lba + 1;
        p->attributes   = e->attributes;
        memcpy(p->type_guid,   e->type_guid,   16);
        memcpy(p->unique_guid, e->unique_guid, 16);
        utf16_to_ascii(e->name_utf16, 36, p->name, (int)sizeof(p->name));
    }

    kfree(table);
    return found;
}

void gpt_dump(int block_id) {
    gpt_partition_t parts[GPT_MAX_PARTITIONS];
    int n = gpt_parse(block_id, parts, GPT_MAX_PARTITIONS);
    if (n < 0) { term_print("[GPT] parse failed.\n"); return; }

    char buf[128];
    /* No proper printf in kernel — itoa each piece. */
    char tmp[32];
    itoa(n, 10, tmp);
    strcpy(buf, "[GPT] ");
    strcat(buf, tmp);
    strcat(buf, " partition(s):\n");
    term_print(buf);

    for (int i = 0; i < n; ++i) {
        gpt_partition_t *p = &parts[i];
        strcpy(buf, "  #");
        itoa(p->index, 10, tmp); strcat(buf, tmp);
        strcat(buf, " name=\""); strcat(buf, p->name); strcat(buf, "\"");
        strcat(buf, " first=");
        itoa((int64_t)p->first_lba, 10, tmp); strcat(buf, tmp);
        strcat(buf, " sectors=");
        itoa((int64_t)p->size_sectors, 10, tmp); strcat(buf, tmp);
        strcat(buf, "\n");
        term_print(buf);
    }
}

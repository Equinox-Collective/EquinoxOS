#ifndef GPT_H
#define GPT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * GPT (GUID Partition Table) parser.
 * Spec: UEFI 2.x, Section 5.3.
 *
 * Layout on disk:
 *   LBA 0          : protective MBR (one entry, type 0xEE, spans entire disk)
 *   LBA 1          : primary GPT header (this struct, 92 bytes meaningful)
 *   LBA 2 .. N     : partition entry array, default 128 entries × 128 bytes
 *                    (so 32 sectors of 512 bytes)
 *   last LBA       : secondary GPT header (mirror)
 */

#define GPT_SIGNATURE 0x5452415020494645ULL /* "EFI PART" little-endian */
#define GPT_REVISION_1_0 0x00010000
#define GPT_MAX_PARTITIONS 128

typedef struct __attribute__((packed)) gpt_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;   /* almost always 128 */
    uint32_t partition_entries_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) gpt_entry_raw {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name_utf16[36];
} gpt_entry_raw_t;

/* Parsed, friendly form. */
typedef struct gpt_partition {
    int      index;             /* 0-based slot                                  */
    uint64_t first_lba;
    uint64_t last_lba;          /* inclusive                                     */
    uint64_t size_sectors;      /* last_lba - first_lba + 1                      */
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    char     name[37];          /* UTF-16 -> ASCII (best-effort, null terminated) */
    uint64_t attributes;
} gpt_partition_t;

/*
 * gpt_parse:
 *   Read GPT structures from block device `block_id` (HAL block-device id) and
 *   fill `out[]` with up to `max_out` used partitions. Returns the count
 *   actually written, or -1 on error.
 *
 *   Does NOT verify CRC32 (kernel has no CRC32 yet) — header signature +
 *   revision + entry sanity are checked instead.
 */
int gpt_parse(int block_id, gpt_partition_t *out, int max_out);

/* Pretty-print all partitions to the kernel terminal. */
void gpt_dump(int block_id);

#endif /* GPT_H */

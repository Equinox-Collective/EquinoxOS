#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>

#define EXT2_MAGIC 0xEF53

// Inode states
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000

#pragma pack(push, 1)

typedef struct {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t r_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    
    // EXT2_DYNAMIC_REV specific
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t features_compat;
    uint32_t features_incompat;
    uint32_t features_ro_compat;
    uint8_t  uuid[16];
    char     volume_name[16];
    char     last_mounted[64];
    uint32_t algo_bitmap;
    
    // Performance hints
    uint8_t  prealloc_blocks;
    uint8_t  prealloc_dir_blocks;
    uint16_t alignment;
    uint8_t  journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
    
    uint32_t hash_seed[4];
    uint8_t  def_hash_version;
    uint8_t  reserved_char_pad;
    uint16_t reserved_word_pad;
    uint32_t default_mount_opts;
    uint32_t first_meta_bg;
    uint8_t  reserved[760];
} ext2_superblock_t;

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t  reserved[12];
} ext2_group_desc_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t  osd2[12];
} ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    // name follows
} ext2_dir_entry_t;

#pragma pack(pop)

uint32_t ext2_resolve_path(const char* path);
uint32_t ext2_read(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer);
uint32_t ext2_write(uint32_t inode_num, uint32_t offset, uint32_t size, uint8_t* buffer);
void ext2_overwrite(const char* name, const char* data, uint32_t size);
void ext2_read_block(uint32_t block, uint8_t* buffer);
void ext2_write_block(uint32_t block, uint8_t *buffer);
void ext2_read_inode(
    uint32_t inode, ext2_inode_t *out_inode);
uint32_t ext2_get_inode_block(ext2_inode_t* inode, uint32_t block);

struct vfs_node* ext2_get_root_node(void);
void ext2_init(void);
void ext2_stress_test_phase1(void);
void ext2_stress_test_phase2(void);
void ext2_stress_test_phase3(void);
void ext2_stress_test_phase4(void);
void ext2_stress_test_phase5(void);

#endif

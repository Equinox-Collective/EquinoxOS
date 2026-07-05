#include "ext2.h"
#include "vfs.h"
#include "../drivers/hardware/disk/ata.h"
#include "../mem/memory.h"
#include "../../syslibc/string.h"

#define EXT2_MAGIC 0xEF53
#define BLOCK_SIZE 4096
#define INODE_SIZE 128
#define INODES_COUNT 2048

#define GROUP_DESC_BLOCK 1
#define BLOCK_BITMAP_BLOCK 2
#define INODE_BITMAP_BLOCK 3
#define INODE_TABLE_BLOCK 4
extern void term_print(const char* str);
// Хелперы для работы с битмапами
static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

static uint32_t align4(uint32_t value) {
    return (value + 3) & ~3;
}

int ext2_format(uint64_t start_lba, uint64_t sector_count) {
    term_print("[mkfs.ext2] Formatting partition...\n");

    uint32_t blocks_count = (sector_count * 512) / BLOCK_SIZE;
    uint32_t inodes_per_group = INODES_COUNT;
    uint32_t blocks_per_group = BLOCK_SIZE * 8; // 32768 блоков в группе

    // Вычисляем размер таблицы иннод в блоках
    uint32_t inode_table_blocks = (INODES_COUNT * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t first_data_block = INODE_TABLE_BLOCK + inode_table_blocks; // Обычно 68 блок
    uint32_t root_dir_block = first_data_block; // 68 блок отдаем под корень

    // Выделяем временные буферы в памяти
    uint8_t *block_bitmap = (uint8_t *)kmalloc(BLOCK_SIZE);
    uint8_t *inode_bitmap = (uint8_t *)kmalloc(BLOCK_SIZE);
    memset(block_bitmap, 0, BLOCK_SIZE);
    memset(inode_bitmap, 0, BLOCK_SIZE);

    // 1. Помечаем метаданные как занятые в битмапе блоков
    // Системные блоки (0..67) + 68 блок (корень)
    for (uint32_t b = 0; b <= root_dir_block; b++) {
        bitmap_set(block_bitmap, b);
    }
    // Помечаем блоки за пределами раздела как занятые
    for (uint32_t b = blocks_count; b < blocks_per_group; b++) {
        bitmap_set(block_bitmap, b);
    }

    // 2. Помечаем системные инноды как занятые (1..10)
    for (uint32_t i = 1; i < 11; i++) {
        bitmap_set(inode_bitmap, i - 1);
    }
    // Инноды за пределами лимита помечаем как занятые
    for (uint32_t i = INODES_COUNT + 1; i <= BLOCK_SIZE * 8; i++) {
        bitmap_set(inode_bitmap, i - 1);
    }
    // Помечаем корневую инноду (2) как занятую
    bitmap_set(inode_bitmap, 2 - 1);

    // Подсчитываем свободные ресурсы
    uint32_t free_blocks = blocks_count - (root_dir_block + 1);
    uint32_t free_inodes = INODES_COUNT - 11; // 10 системных + 1 корневая

    // 3. Создаем Суперблок
    uint8_t *sb_buf = (uint8_t *)kmalloc(1024);
    memset(sb_buf, 0, 1024);
    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;

    sb->inodes_count = INODES_COUNT;
    sb->blocks_count = blocks_count;
    sb->r_blocks_count = 0;
    sb->free_blocks_count = free_blocks;
    sb->free_inodes_count = free_inodes;
    sb->first_data_block = 0; // Для 4КБ блоков первый дата-блок всегда 0 в заголовке
    sb->log_block_size = 2;   // 1024 << 2 = 4096 байт
    sb->log_frag_size = 2;
    sb->blocks_per_group = blocks_per_group;
    sb->frags_per_group = blocks_per_group;
    sb->inodes_per_group = inodes_per_group;
    sb->mtime = 0;
    sb->wtime = 0;
    sb->mnt_count = 0;
    sb->max_mnt_count = 0xFFFF;
    sb->magic = EXT2_MAGIC;
    sb->state = 1; // Clean
    sb->errors = 1; // Continue
    sb->minor_rev_level = 0;
    sb->lastcheck = 0;
    sb->checkinterval = 0;
    sb->creator_os = 0; // Linux/Equinox
    sb->rev_level = 1;  // Dynamic revision
    sb->def_resuid = 0;
    sb->def_resgid = 0;
    sb->first_ino = 11; // First non-reserved inode
    sb->inode_size = INODE_SIZE;
    memcpy(sb->volume_name, "EquinoxOS EXT2", 14);

    // 4. Создаем Дескриптор Группы Блоков
    ext2_group_desc_t bgd;
    memset(&bgd, 0, sizeof(ext2_group_desc_t));
    bgd.block_bitmap = BLOCK_BITMAP_BLOCK;
    bgd.inode_bitmap = INODE_BITMAP_BLOCK;
    bgd.inode_table = INODE_TABLE_BLOCK;
    bgd.free_blocks_count = free_blocks;
    bgd.free_inodes_count = free_inodes;
    bgd.used_dirs_count = 1; // Корень - это директория

    // 5. Создаем корневой каталог (блок 68)
    uint8_t *dir_buf = (uint8_t *)kmalloc(BLOCK_SIZE);
    memset(dir_buf, 0, BLOCK_SIZE);

    // Запись "."
    ext2_dir_entry_t *de_dot = (ext2_dir_entry_t *)dir_buf;
    de_dot->inode = 2; // ROOT_INODE
    de_dot->rec_len = 12; // align4(8 + 1) = 12
    de_dot->name_len = 1;
    de_dot->file_type = 2; // Directory
    memcpy((uint8_t*)de_dot + sizeof(ext2_dir_entry_t), ".", 1);

    // Запись ".." (занимает всё оставшееся место в блоке)
    ext2_dir_entry_t *de_dotdot = (ext2_dir_entry_t *)(dir_buf + 12);
    de_dotdot->inode = 2;
    de_dotdot->rec_len = BLOCK_SIZE - 12;
    de_dotdot->name_len = 2;
    de_dotdot->file_type = 2;
    memcpy((uint8_t*)de_dotdot + sizeof(ext2_dir_entry_t), "..", 2);

    // 6. Создаем корневую инноду (номер 2)
    ext2_inode_t root_inode;
    memset(&root_inode, 0, sizeof(ext2_inode_t));
    root_inode.mode = 0x41ED; // Directory, rwxr-xr-x
    root_inode.uid = 0;
    root_inode.size = BLOCK_SIZE;
    root_inode.gid = 0;
    root_inode.links_count = 2; // "." и ".."
    root_inode.blocks = BLOCK_SIZE / 512; // В секторах
    root_inode.block[0] = root_dir_block; // Указывает на 68 блок

    // === ЗАПИСЬ НА ДИСК ===

    // Запись суперблока (смещение 1024 байта от начала раздела)
    write_sectors_ata_pio((uintptr_t)sb_buf, start_lba + 2, 2); // 1024 байта = 2 сектор

    // Запись Group Descriptor (Блок 1)
    uint8_t *bgd_block_buf = (uint8_t *)kmalloc(BLOCK_SIZE);
    memset(bgd_block_buf, 0, BLOCK_SIZE);
    memcpy(bgd_block_buf, &bgd, sizeof(ext2_group_desc_t));
    write_sectors_ata_pio((uintptr_t)bgd_block_buf, start_lba + GROUP_DESC_BLOCK * 8, 8);

    // Запись битмапов (Блоки 2 и 3)
    write_sectors_ata_pio((uintptr_t)block_bitmap, start_lba + BLOCK_BITMAP_BLOCK * 8, 8);
    write_sectors_ata_pio((uintptr_t)inode_bitmap, start_lba + INODE_BITMAP_BLOCK * 8, 8);

    // Инициализация таблицы иннод нулями (Блок 4 .. 4+inode_table_blocks)
    uint8_t *empty_block = (uint8_t *)kmalloc(BLOCK_SIZE);
    memset(empty_block, 0, BLOCK_SIZE);
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        write_sectors_ata_pio((uintptr_t)empty_block, start_lba + (INODE_TABLE_BLOCK + i) * 8, 8);
    }

    // Записываем корневую инноду (номер 2) в таблицу иннод
    // Иннода 2 находится в первом блоке таблицы иннод, смещение = (2 - 1) * 128 = 128 байт
    uint8_t *first_inode_block = (uint8_t *)kmalloc(BLOCK_SIZE);
    memset(first_inode_block, 0, BLOCK_SIZE);
    memcpy(first_inode_block + INODE_SIZE, &root_inode, sizeof(ext2_inode_t));
    write_sectors_ata_pio((uintptr_t)first_inode_block, start_lba + INODE_TABLE_BLOCK * 8, 8);

    // Записываем корневой каталог (Блок 68)
    write_sectors_ata_pio((uintptr_t)dir_buf, start_lba + root_dir_block * 8, 8);

    // Освобождаем память
    kfree(block_bitmap);
    kfree(inode_bitmap);
    kfree(sb_buf);
    kfree(bgd_block_buf);
    kfree(dir_buf);
    kfree(empty_block);
    kfree(first_inode_block);

    term_print("[mkfs.ext2] Formatting complete! Ext2 partition is ready.\n");
    return 0;
}
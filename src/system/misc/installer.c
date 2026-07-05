#include "../fs/vfs.h"
#include "../fs/gpt.h"
#include "../fs/ext2.h"
#include "../fs/fat32.h"
#include "../../syslibc/stdio.h"
#include "../../syslibc/string.h"

extern void term_print(const char* str);
extern void ext2_set_partition(uint64_t lba);
extern int ext2_format(uint64_t start_lba, uint64_t sector_count);
extern void fat32_init(void);

// Вспомогательная функция для создания родительских директорий по пути (например, /mnt/target/bin)
static void ensure_directory(const char *path) {
    char temp[256];
    strncpy(temp, path, sizeof(temp) - 1);
    temp[sizeof(temp)-1] = '\0';

    // Отрезаем имя файла, оставляем только путь к папке
    char *last_slash = strrchr(temp, '/');
    if (last_slash) {
        *last_slash = '\0';
        // Если такой папки еще нет в VFS, создаем её рекурсивно
        if (!vfs_dir_exists(temp)) {
            // Если нужно, можно сделать mkdir для каждого уровня, но у нас плоская структура
            vfs_mkdir(temp);
        }
    }
}

// Функция копирования файла средствами VFS
int copy_file(const char *src, const char *dst) {
    vfs_file_t *sf = vfs_open(src, VFS_O_RDONLY);
    if (!sf) return -1;

    // Гарантируем, что папка назначения существует
    ensure_directory(dst);

    vfs_file_t *df = vfs_open(dst, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (!df) {
        vfs_close(sf);
        return -1;
    }

    uint8_t buffer[4096];
    int bytes_read;
    while ((bytes_read = vfs_read(sf, buffer, sizeof(buffer))) > 0) {
        vfs_write(df, buffer, bytes_read);
    }

    vfs_close(sf);
    vfs_close(df);
    return 0;
}

void install_equinox_os(void) {
    term_print("\n=== EquinoxOS Universal Installer ===\n");

    // 1. Парсим GPT
    gpt_partition_t parts[GPT_MAX_PARTITIONS];
    int part_count = gpt_parse(0, parts, GPT_MAX_PARTITIONS);
    if (part_count < 0) {
        term_print("Error: Could not parse GPT. Installation aborted.\n");
        return;
    }

    // 2. Ищем ESP (FAT32)
    int esp_idx = gpt_find_esp(0, parts, part_count);
    if (esp_idx == -1) {
        term_print("Error: EFI System Partition (ESP) not found!\n");
        return;
    }
    gpt_partition_t *esp = &parts[esp_idx];

    // 3. Выбираем раздел под Root
    int root_idx = -1;
    for (int i = 0; i < part_count; i++) {
        if (i != esp_idx && parts[i].size_sectors > 100000) {
            root_idx = i;
            break;
        }
    }
    if (root_idx == -1) {
        term_print("Error: Target partition for OS Root not found!\n");
        return;
    }
    gpt_partition_t *root_part = &parts[root_idx];

    // 4. Форматируем Ext2 раздел под ОС
    ext2_format(root_part->first_lba, root_part->size_sectors);

    // 5. Монтируем целевой раздел Ext2 в /mnt/target
    ext2_set_partition(root_part->first_lba);
    vfs_node_t *ext2_root = ext2_get_root_node();
    vfs_mount("/mnt/target", ext2_root);

    // 6. Монтируем ESP (FAT32) в /mnt/esp
    fat32_init();
    vfs_node_t *fat_root = fat32_get_root_node();
    vfs_mount("/mnt/esp", fat_root);

    // 7. Читаем манифест установки с загрузочной флешки
    vfs_file_t *manifest = vfs_open("/etc/install_manifest.txt", VFS_O_RDONLY);
    if (!manifest) {
        term_print("Error: /etc/install_manifest.txt not found! Installation aborted.\n");
        return;
    }

    term_print("Copying system files based on manifest...\n");

    char line_buf[256];
    int line_len = 0;
    char c;

    // Построчно читаем манифест
    while (vfs_read(manifest, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            if (line_len > 0) {
                line_buf[line_len] = '\0';

                // Генерируем пути копирования
                // Источник: "/bin/sh.elf"
                // Назначение: "/mnt/target/bin/sh.elf"
                char dest_path[512];
                sprintf(dest_path, "/mnt/target%s", line_buf);

                char log_msg[512];
                sprintf(log_msg, "  -> Copying: %s\n", line_buf);
                term_print(log_msg);

                if (copy_file(line_buf, dest_path) != 0) {
                    char err_msg[512];
                    sprintf(err_msg, "  [!] Error copying %s\n", line_buf);
                    term_print(err_msg);
                }
                line_len = 0;
            }
        } else if (line_len < 254) {
            line_buf[line_len++] = c;
        }
    }
    vfs_close(manifest);

    // 8. Копируем файлы загрузчика Limine на ESP
    term_print("Installing Limine Bootloader to ESP...\n");
    
    vfs_mkdir("/mnt/esp/EFI");
    vfs_mkdir("/mnt/esp/EFI/BOOT");
    vfs_mkdir("/mnt/esp/res");
    vfs_mkdir("/mnt/esp/sys");

    copy_file("/boot/limine/BOOTX64.EFI", "/mnt/esp/EFI/BOOT/BOOTX64.EFI");
    copy_file("/sys/kernel.elf",          "/mnt/esp/sys/kernel.elf");
    copy_file("/res/BG.BMP",              "/mnt/esp/res/BG.BMP");

    // Записываем новый limine.conf для жесткого диска
    const char *new_limine_conf = 
        "timeout: 3\n"
        "backdrop: ff0b1220\n"
        "term_background: ff0b1220\n"
        "term_foreground: ffffff\n"
        "term_wallpaper: boot():/res/BG.BMP\n"
        "wallpaper: boot():/res/BG.BMP\n"
        "/EquinoxOS\n"
        "    protocol: limine\n"
        "    path: boot():/sys/kernel.elf\n"
        "    resolution: 1920x1080x32\n"
        "    module_path: boot():/res/font.psf\n";

    vfs_write_file("/mnt/esp/EFI/BOOT/limine.conf", (const uint8_t*)new_limine_conf, strlen(new_limine_conf));

    term_print("\nInstallation complete! Please reboot.\n");
}
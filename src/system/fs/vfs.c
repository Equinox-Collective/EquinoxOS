#include "vfs.h"
#include "../mem/memory.h"
#include "../../syslibc/string.h"
#include "../../syslibc/stdio.h"

vfs_node_t* vfs_root = NULL;

void vfs_init() {
    vfs_root = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    memset(vfs_root, 0, sizeof(vfs_node_t));
    strcpy(vfs_root->name, "root");
    vfs_root->flags = 0x01; // Папка
}

// Регистрация устройства в корне (упрощенно)
void vfs_register_device(vfs_node_t* node) {
    if (!node) return;
    node->next = vfs_root->next;
    vfs_root->next = node;
}

// Поиск узла по имени
vfs_node_t* vfs_find(const char* name) {
    vfs_node_t* curr = vfs_root->next;
    while (curr) {
        if (strcmp(curr->name, name) == 0) return curr;
        curr = curr->next;
    }
    return NULL;
}

// Системные вызовы (внутриядерные)
uint32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node && node->write) {
        return node->write(node, offset, size, buffer);
    }
    return 0;
}

extern void term_print(const char* str);

void vfs_ls(void) {
    vfs_node_t* dev = vfs_root->next;
    if (!dev) {
        term_print("VFS: No devices registered.\n");
        return;
    }
    while (dev) {
        term_print("--- Volume: "); term_print(dev->name); term_print(" ---\n");
        if (dev->readdir) {
            for (int i = 0; i < 256; i++) {
                vfs_dirent_t* de = dev->readdir(dev, i);
                if (!de) break;
                term_print("  ");
                term_print(de->name);
                
                char sizebuf[32];
                sprintf(sizebuf, " (%d bytes)\n", de->size);
                term_print(sizebuf);
            }
        } else {
            term_print("  (No readdir support)\n");
        }
        dev = dev->next;
    }
}

uint8_t* vfs_read_file(const char* name, uint32_t* out_size) {
    vfs_node_t* dev = vfs_root->next;
    while (dev) {
        // Быстрый путь: O(n) поиск за один проход по каталогу. Раньше шёл
        // O(n^2) перебор readdir(index) ниже — он ОЧЕНЬ медленный на ATA PIO.
        if (dev->finddir && dev->read) {
            vfs_node_t* fn = dev->finddir(dev, (char*)name);
            if (fn) {
                *out_size = fn->size;
                uint8_t* buf = kmalloc(fn->size);
                if (buf && dev->read(fn, 0, fn->size, buf) > 0) return buf;
                if (buf) kfree(buf);
            }
            // finddir авторитетен для этого устройства: не найдено — следующее.
            dev = dev->next;
            continue;
        }
        if (dev->readdir && dev->read) {
            for (int i = 0; i < 256; i++) {
                vfs_dirent_t* de = dev->readdir(dev, i);
                if (!de) break;
                if (strcmp(de->name, name) == 0) {
                    *out_size = de->size;
                    uint8_t* buf = kmalloc(*out_size);
                    vfs_node_t file_node;
                    memset(&file_node, 0, sizeof(vfs_node_t));
                    file_node.inode = de->inode;
                    strcpy(file_node.name, de->name);
                    if (dev->read(&file_node, 0, de->size, buf) > 0) return buf;
                    kfree(buf);
                }
            }
        }
        dev = dev->next;
    }
    return NULL;
}

uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (node && node->read) {
        return node->read(node, offset, size, buffer);
    }
    return 0;
}

/* ===== Этап 3: нормализация путей и проверка каталогов (cwd) ============= */

int vfs_normalize(const char* cwd, const char* path, char* out, int outsz) {
    if (!out || outsz < 2) return -1;
    /* 1. Собираем "сырой" абсолютный путь во временном буфере. */
    char raw[512];
    int n = 0;
    if (path && path[0] == '/') {
        /* абсолютный — cwd игнорируется */
    } else {
        const char* c = (cwd && cwd[0]) ? cwd : "/";
        for (int i = 0; c[i] && n < (int)sizeof(raw) - 1; i++) raw[n++] = c[i];
        if (n == 0 || raw[n - 1] != '/') {
            if (n < (int)sizeof(raw) - 1) raw[n++] = '/';
        }
    }
    if (path) {
        for (int i = 0; path[i] && n < (int)sizeof(raw) - 1; i++) raw[n++] = path[i];
    }
    raw[n] = '\0';

    /* 2. Токенизируем по '/', сворачивая "." и "..". Стек компонент в out. */
    /* offs[] хранит позиции начала каждой компоненты в out (для pop ".."). */
    int offs[64];
    int depth = 0;
    int olen = 0;
    out[0] = '\0';
    int i = 0;
    while (raw[i]) {
        while (raw[i] == '/') i++;          /* пропускаем слэши */
        if (!raw[i]) break;
        int start = i;
        while (raw[i] && raw[i] != '/') i++;
        int complen = i - start;
        if (complen == 1 && raw[start] == '.') continue;          /* "." */
        if (complen == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            if (depth > 0) { depth--; olen = offs[depth]; out[olen] = '\0'; }
            continue;                                              /* ".." */
        }
        if (depth >= 64) return -1;
        if (olen + 1 + complen >= outsz) return -1;
        offs[depth++] = olen;
        out[olen++] = '/';
        for (int k = 0; k < complen; k++) out[olen++] = raw[start + k];
        out[olen] = '\0';
    }
    if (olen == 0) { out[0] = '/'; out[1] = '\0'; olen = 1; }
    return olen;
}

int vfs_dir_exists(const char* logical) {
    if (!logical) return 0;
    if (logical[0] == '/' && logical[1] == '\0') return 1;     /* корень */

    /* prefix = logical без ведущего '/', плюс завершающий '/'. */
    const char* p = logical;
    while (*p == '/') p++;
    char prefix[256];
    int n = 0;
    for (; p[n] && n < (int)sizeof(prefix) - 2; n++) prefix[n] = p[n];
    prefix[n++] = '/';
    prefix[n] = '\0';

    vfs_node_t* dev = vfs_root ? vfs_root->next : NULL;
    while (dev) {
        if (dev->readdir) {
            for (int idx = 0; idx < 1024; idx++) {
                vfs_dirent_t* de = dev->readdir(dev, idx);
                if (!de) break;
                /* запись начинается на prefix? */
                int m = 0;
                while (prefix[m] && de->name[m] == prefix[m]) m++;
                if (prefix[m] == '\0') return 1;
            }
        }
        dev = dev->next;
    }
    return 0;
}
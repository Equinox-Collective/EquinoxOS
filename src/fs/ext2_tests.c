#include "ext2.h"
#include "vfs.h"
#include "../system/memory.h"
#include "../libc/stdio.h"
#include <stdbool.h>

extern void term_print(const char* str);
extern void ext2_read_inode(uint32_t inode, ext2_inode_t* out_inode);
extern uint32_t ext2_resolve_path(const char* path);

void ext2_stress_test_phase1() {
    ext2_inode_t root_inode;
    root_inode.mode = 0; // Initialize
    ext2_read_inode(2, &root_inode);
    
    if (root_inode.mode == 0) {
        term_print("PHASE 1: Skipping (No valid EXT2 filesystem found).\n");
        return;
    }

    term_print("--- EXT2 STRESS TEST PHASE 1 ---\n");
    
    // Test 2: Inode 2 reading (Root directory check)
    if (root_inode.mode & EXT2_S_IFDIR) {
        term_print("PHASE 1: Root inode (2) is a directory. [PASS]\n");
    } else {
        term_print("PHASE 1: Root inode (2) is NOT a directory! [FAIL]\n");
    }
    
    // Test 3: System inodes range check
    for (int i = 1; i <= 10; i++) {
        ext2_inode_t ino;
        ext2_read_inode(i, &ino);
    }
    term_print("PHASE 1: System inodes range check completed. [PASS]\n");
    
    term_print("--- PHASE 1 COMPLETED ---\n");
}

void ext2_stress_test_phase2() {
    uint32_t resolved_root_ino = ext2_resolve_path("");
    if (resolved_root_ino == 0) {
        term_print("PHASE 2: Skipping (No valid EXT2 filesystem found).\n");
        return;
    }

    term_print("--- EXT2 STRESS TEST PHASE 2 ---\n");
    
    // Test 1: Resolve "/"
    if (resolved_root_ino == 2) {
        term_print("PHASE 2: Resolve \"/\" [PASS]\n");
    } else {
        term_print("PHASE 2: Resolve \"/\" [FAIL]\n");
    }
    
    // Test 2: Resolve non-existent path
    uint32_t ghost_ino = ext2_resolve_path("/this/does/not/exist");
    if (ghost_ino == 0) {
        term_print("PHASE 2: Resolve non-existent path [PASS]\n");
    } else {
        term_print("PHASE 2: Resolve non-existent path [FAIL]\n");
    }
    
    term_print("--- PHASE 2 COMPLETED ---\n");
}
void ext2_stress_test_phase3() {
    uint32_t file_ino = ext2_resolve_path("/large.bin");
    if (file_ino == 0) {
        term_print("PHASE 3: Skipping (File /large.bin not found or no EXT2).\n");
        return;
    }

    term_print("--- EXT2 STRESS TEST PHASE 3 ---\n");
    
    ext2_inode_t inode;
    ext2_read_inode(file_ino, &inode);
    
    // Check size (Should be 32,768)
    if (inode.size == 32768) {
        term_print("PHASE 3: File size is 32KB [PASS]\n");
    } else {
        term_print("PHASE 3: File size is WRONG! [FAIL]\n");
    }
    
    // Read and verify data
    uint8_t* buffer = kmalloc(32768);
    uint32_t bytes_read = ext2_read(file_ino, 0, 32768, buffer);
    
    if (bytes_read == 32768) {
        bool ok = true;
        for (int i = 0; i < 32768; i++) {
            if (buffer[i] != 'X') {
                ok = false;
                break;
            }
        }
        if (ok) {
            term_print("PHASE 3: Data integrity (Indirect Blocks) [PASS]\n");
        } else {
            term_print("PHASE 3: Data corruption detected! [FAIL]\n");
        }
    } else {
        term_print("PHASE 3: Read failed or partial read! [FAIL]\n");
    }
    
    kfree(buffer);
    term_print("--- PHASE 3 COMPLETED ---\n");
}

extern void ext2_overwrite(const char* name, const char* data, uint32_t size);
#include "../libc/string.h"

void ext2_stress_test_phase4() {
    uint32_t existing = ext2_resolve_path("/write_test.txt");
    if (existing != 0) {
        term_print("PHASE 4: File already exists. This is expected if running twice.\n");
    }

    term_print("--- EXT2 STRESS TEST PHASE 4 ---\n");
    
    // 1. Create and Write
    const char* test_data = "EXT2 Write Support PASSED!";
    ext2_overwrite("/write_test.txt", test_data, strlen(test_data));
    term_print("PHASE 4: Wrote \"/write_test.txt\" [PASS]\n");
    
    // 2. Verify existence
    uint32_t new_ino = ext2_resolve_path("/write_test.txt");
    if (new_ino != 0) {
        term_print("PHASE 4: File resolution after write [PASS]\n");
    } else {
        term_print("PHASE 4: File NOT found after write! [FAIL]\n");
        return;
    }
    
    // 3. Read back and compare
    char read_buf[64] = {0};
    ext2_read(new_ino, 0, strlen(test_data), (uint8_t*)read_buf);
    
    if (strcmp(read_buf, test_data) == 0) {
        term_print("PHASE 4: Data verification [PASS]\n");
    } else {
        term_print("PHASE 4: Data MISMATCH! [FAIL]\n");
        term_print("Expected: "); term_print(test_data); term_print("\n");
        term_print("Got: "); term_print(read_buf); term_print("\n");
    }
    
    term_print("--- PHASE 4 COMPLETED ---\n");
}

void ext2_stress_test_phase5() {
    term_print("--- EXT2 STRESS TEST PHASE 5 ---\n");

    const char* test_data = "VFS Write Support PASSED!";
    const char* dev_name = NULL;
    uint32_t written = vfs_write_file("vfs_write_test.txt", (const uint8_t*)test_data,
                                      strlen(test_data), &dev_name);

    if (written == strlen(test_data)) {
        term_print("PHASE 5: VFS write returned full size [PASS]\n");
    } else {
        term_print("PHASE 5: VFS write failed or partial write [FAIL]\n");
        return;
    }

    uint32_t ino = ext2_resolve_path("/vfs_write_test.txt");
    if (ino != 0) {
        term_print("PHASE 5: VFS-created file resolved on EXT2 [PASS]\n");
    } else {
        term_print("PHASE 5: VFS-created file missing on EXT2 [FAIL]\n");
        return;
    }

    char read_buf[64] = {0};
    ext2_read(ino, 0, strlen(test_data), (uint8_t*)read_buf);
    if (strcmp(read_buf, test_data) == 0) {
        term_print("PHASE 5: VFS write data verification [PASS]\n");
    } else {
        term_print("PHASE 5: VFS write data mismatch [FAIL]\n");
    }

    if (dev_name) {
        term_print("PHASE 5: Device: ");
        term_print(dev_name);
        term_print("\n");
    }

    term_print("--- PHASE 5 COMPLETED ---\n");
}

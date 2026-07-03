# EXT2 Filesystem Implementation Plan

This document outlines the strategy for implementing EXT2 filesystem support in EquinoxOS.

## Phase 1: Basic Infrastructure & Superblock
**Goal**: Read and validate the EXT2 Superblock and Block Group Descriptors.

- [x] Define EXT2 data structures in `src/fs/ext2.h`.
- [x] Implement `ext2_mount` function (internal `ext2_init`).
- [x] Read Superblock (located at 1024 bytes offset).
- [x] Read Block Group Descriptor Table.
- [x] **Stress Test**:
    - Mount validation (Magic number).
    - Basic Inode reading (Root inode).

## Phase 2: Inode & Directory Traversal
**Goal**: Locate inodes and navigate the directory tree.

- [x] Implement `ext2_read_inode`.
- [x] Implement `ext2_find_entry` (search for a filename in a directory).
- [x] Implement path resolution (e.g., `/usr/bin/shell`).
- [x] Implement basic `ext2_read` for file data (direct blocks only).
- [ ] **Stress Test**:
    - Resolve paths with multiple tokens.
    - Resolve non-existent paths.
    - [x] Basic path resolution tests added to `ext2_tests.c`.

## Phase 3: Advanced Reading (Indirect Blocks)
**Goal**: Support large files.

- [ ] Implement support for Indirect, Doubly Indirect, and Triply Indirect blocks.
- [ ] **Stress Test**:
    - Read a file larger than 12 blocks (Indirect).
    - Read a file larger than 12 + 256 blocks (Double Indirect).

## Phase 4: Write Support (The Hard Part)
**Goal**: Create, write, and delete files/directories.

- [ ] Implement Block and Inode Bitmaps management.
- [ ] Implement `ext2_write_inode`.
- [ ] Implement `ext2_create` and `ext2_mkdir`.
- [ ] Implement `ext2_write` for data.
- [ ] Implement `ext2_unlink` (delete).
- [ ] **Stress Test**:
    - "The Bomb": Create 1,000 files in a single directory.
    - Full disk test: Write until no space is left.

## Phase 5: VFS Integration
**Goal**: Seamlessly use EXT2 through the standard VFS interface.

- [ ] Update `vfs_node_t` for better FS abstraction.
- [ ] Register EXT2 as a filesystem type.
- [ ] Update GUI (Explorer) to use VFS entries.

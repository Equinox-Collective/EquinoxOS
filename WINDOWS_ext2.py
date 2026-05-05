import os
import struct

BLOCK_SIZE = 4096
INODE_SIZE = 128
IMAGE_SIZE_MB = 64
IMAGE_SIZE = IMAGE_SIZE_MB * 1024 * 1024
BLOCKS_COUNT = IMAGE_SIZE // BLOCK_SIZE
INODES_COUNT = 2048
BLOCKS_PER_GROUP = BLOCK_SIZE * 8
INODES_PER_GROUP = INODES_COUNT
EXT2_MAGIC = 0xEF53

GROUP_DESC_BLOCK = 1
BLOCK_BITMAP_BLOCK = 2
INODE_BITMAP_BLOCK = 3
INODE_TABLE_BLOCK = 4
INODE_TABLE_BLOCKS = (INODES_COUNT * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE
FIRST_DATA_BLOCK = INODE_TABLE_BLOCK + INODE_TABLE_BLOCKS
ROOT_INODE = 2
FIRST_USER_INODE = 11


def align4(value):
    return (value + 3) & ~3


def bitmap_set(bitmap, index):
    bitmap[index // 8] |= 1 << (index % 8)


def bitmap_is_set(bitmap, index):
    return (bitmap[index // 8] & (1 << (index % 8))) != 0


def bitmap_count_used(bitmap, limit):
    return sum(1 for index in range(limit) if bitmap_is_set(bitmap, index))


class Ext2Generator:
    def __init__(self, filename):
        self.filename = filename
        self.data = bytearray(IMAGE_SIZE)
        self.block_bitmap = bytearray(BLOCK_SIZE)
        self.inode_bitmap = bytearray(BLOCK_SIZE)
        self.next_block = FIRST_DATA_BLOCK
        self.next_inode = FIRST_USER_INODE
        self.root_entries = []

        for block in range(FIRST_DATA_BLOCK):
            self.mark_block_used(block)

        for block in range(BLOCKS_COUNT, BLOCKS_PER_GROUP):
            self.mark_block_used(block)

        for inode in range(1, FIRST_USER_INODE):
            self.mark_inode_used(inode)

        for inode in range(INODES_COUNT + 1, BLOCK_SIZE * 8 + 1):
            self.mark_inode_used(inode)

    def block_offset(self, block):
        return block * BLOCK_SIZE

    def mark_block_used(self, block):
        bitmap_set(self.block_bitmap, block)

    def mark_inode_used(self, inode):
        bitmap_set(self.inode_bitmap, inode - 1)

    def alloc_block(self):
        for block in range(self.next_block, BLOCKS_COUNT):
            if not bitmap_is_set(self.block_bitmap, block):
                self.mark_block_used(block)
                self.next_block = block + 1
                return block
        raise RuntimeError("EXT2 image is out of blocks")

    def alloc_inode(self):
        if self.next_inode > INODES_COUNT:
            raise RuntimeError("EXT2 image is out of inodes")

        inode = self.next_inode
        self.next_inode += 1
        self.mark_inode_used(inode)
        return inode

    def write_block(self, block, payload):
        offset = self.block_offset(block)
        size = min(len(payload), BLOCK_SIZE)
        self.data[offset:offset + size] = payload[:size]

    def write_inode(self, inode, mode, size, block_pointers, links_count=1, blocks_used=None):
        if len(block_pointers) != 15:
            raise ValueError("EXT2 inode needs exactly 15 block pointers")

        inode_offset = self.block_offset(INODE_TABLE_BLOCK) + (inode - 1) * INODE_SIZE
        if blocks_used is None:
            blocks_used = sum(1 for block in block_pointers if block)

        sectors_used = blocks_used * (BLOCK_SIZE // 512)

        struct.pack_into("<HHLLLLLHHLLL", self.data, inode_offset,
                         mode, 0, size, 0, 0, 0, 0, 0,
                         links_count, sectors_used, 0, 0)
        struct.pack_into("<15L", self.data, inode_offset + 40, *block_pointers)

    def write_pointer_block(self, pointers):
        block = self.alloc_block()
        payload = bytearray(BLOCK_SIZE)
        struct.pack_into("<%dL" % len(pointers), payload, 0, *pointers)
        self.write_block(block, payload)
        return block

    def make_file_blocks(self, content):
        if not content:
            return [0] * 15, 0

        data_blocks = []
        for offset in range(0, len(content), BLOCK_SIZE):
            block = self.alloc_block()
            self.write_block(block, content[offset:offset + BLOCK_SIZE])
            data_blocks.append(block)

        inode_blocks = [0] * 15
        blocks_used = len(data_blocks)
        ptrs_per_block = BLOCK_SIZE // 4

        direct = data_blocks[:12]
        inode_blocks[:len(direct)] = direct
        remaining = data_blocks[12:]

        if remaining:
            indirect_ptrs = remaining[:ptrs_per_block]
            remaining = remaining[ptrs_per_block:]
            inode_blocks[12] = self.write_pointer_block(indirect_ptrs)
            blocks_used += 1

        if remaining:
            double_indirect_ptrs = []
            while remaining:
                indirect_ptrs = remaining[:ptrs_per_block]
                remaining = remaining[ptrs_per_block:]
                double_indirect_ptrs.append(self.write_pointer_block(indirect_ptrs))
                blocks_used += 1

            inode_blocks[13] = self.write_pointer_block(double_indirect_ptrs)
            blocks_used += 1

        return inode_blocks, blocks_used

    def add_file(self, name, content):
        inode = self.alloc_inode()
        block_pointers, blocks_used = self.make_file_blocks(content)
        self.write_inode(inode, 0x81A4, len(content), block_pointers, 1, blocks_used)
        self.root_entries.append((inode, name, 1))
        print(f"  + {name} ({len(content)} bytes) -> Inode {inode}")

    def write_root_directory(self):
        root_block = self.alloc_block()
        entries = [(ROOT_INODE, ".", 2), (ROOT_INODE, "..", 2)] + self.root_entries

        directory = bytearray(BLOCK_SIZE)
        offset = 0
        for index, (inode, name, file_type) in enumerate(entries):
            name_bytes = name.encode("ascii")
            min_len = align4(8 + len(name_bytes))
            rec_len = min_len
            if index == len(entries) - 1:
                rec_len = BLOCK_SIZE - offset

            if offset + rec_len > BLOCK_SIZE:
                raise RuntimeError("Root directory does not fit in one block")

            struct.pack_into("<LHBB", directory, offset, inode, rec_len, len(name_bytes), file_type)
            directory[offset + 8:offset + 8 + len(name_bytes)] = name_bytes
            offset += rec_len

        self.write_block(root_block, directory)

        root_blocks = [0] * 15
        root_blocks[0] = root_block
        self.write_inode(ROOT_INODE, 0x41ED, BLOCK_SIZE, root_blocks, 2, 1)

    def write_superblock(self):
        offset = 1024
        free_blocks = BLOCKS_COUNT - bitmap_count_used(self.block_bitmap, BLOCKS_COUNT)
        free_inodes = INODES_COUNT - bitmap_count_used(self.inode_bitmap, INODES_COUNT)

        struct.pack_into("<L", self.data, offset + 0, INODES_COUNT)
        struct.pack_into("<L", self.data, offset + 4, BLOCKS_COUNT)
        struct.pack_into("<L", self.data, offset + 8, 0)
        struct.pack_into("<L", self.data, offset + 12, free_blocks)
        struct.pack_into("<L", self.data, offset + 16, free_inodes)
        struct.pack_into("<L", self.data, offset + 20, 0)
        struct.pack_into("<L", self.data, offset + 24, 2)
        struct.pack_into("<L", self.data, offset + 28, 2)
        struct.pack_into("<L", self.data, offset + 32, BLOCKS_PER_GROUP)
        struct.pack_into("<L", self.data, offset + 36, BLOCKS_PER_GROUP)
        struct.pack_into("<L", self.data, offset + 40, INODES_PER_GROUP)
        struct.pack_into("<L", self.data, offset + 44, 0)
        struct.pack_into("<L", self.data, offset + 48, 0)
        struct.pack_into("<H", self.data, offset + 52, 0)
        struct.pack_into("<H", self.data, offset + 54, 0xFFFF)
        struct.pack_into("<H", self.data, offset + 56, EXT2_MAGIC)
        struct.pack_into("<H", self.data, offset + 58, 1)
        struct.pack_into("<H", self.data, offset + 60, 1)
        struct.pack_into("<H", self.data, offset + 62, 0)
        struct.pack_into("<L", self.data, offset + 64, 0)
        struct.pack_into("<L", self.data, offset + 68, 0)
        struct.pack_into("<L", self.data, offset + 72, 0)
        struct.pack_into("<L", self.data, offset + 76, 1)
        struct.pack_into("<H", self.data, offset + 80, 0)
        struct.pack_into("<H", self.data, offset + 82, 0)
        struct.pack_into("<L", self.data, offset + 84, FIRST_USER_INODE)
        struct.pack_into("<H", self.data, offset + 88, INODE_SIZE)
        struct.pack_into("<H", self.data, offset + 90, 0)
        self.data[offset + 120:offset + 136] = b"EquinoxOS EXT2\0\0"

    def write_group_descriptor(self):
        free_blocks = BLOCKS_COUNT - bitmap_count_used(self.block_bitmap, BLOCKS_COUNT)
        free_inodes = INODES_COUNT - bitmap_count_used(self.inode_bitmap, INODES_COUNT)

        offset = self.block_offset(GROUP_DESC_BLOCK)
        struct.pack_into("<LLLHHH", self.data, offset,
                         BLOCK_BITMAP_BLOCK,
                         INODE_BITMAP_BLOCK,
                         INODE_TABLE_BLOCK,
                         free_blocks,
                         free_inodes,
                         1)

    def flush_bitmaps(self):
        self.write_block(BLOCK_BITMAP_BLOCK, self.block_bitmap)
        self.write_block(INODE_BITMAP_BLOCK, self.inode_bitmap)

    def generate(self, source_dir):
        print(f"[EXT2GEN] Creating {self.filename} from {source_dir}...")

        added_names = set()
        for fname in sorted(os.listdir(source_dir)):
            fpath = os.path.join(source_dir, fname)
            if os.path.isfile(fpath):
                with open(fpath, "rb") as f:
                    self.add_file(fname, f.read())
                added_names.add(fname.upper())

        sample_bmp = os.path.join("logos", "equinox_high_quality_200x200.bmp")
        if "LOGO.BMP" not in added_names and os.path.isfile(sample_bmp):
            with open(sample_bmp, "rb") as f:
                self.add_file("LOGO.BMP", f.read())

        self.add_file("large.bin", b"X" * 32768)
        self.write_root_directory()
        self.flush_bitmaps()
        self.write_group_descriptor()
        self.write_superblock()

        with open(self.filename, "wb") as f:
            f.write(self.data)
        print("[EXT2GEN] Done.")


if __name__ == "__main__":
    gen = Ext2Generator("hdd.img")
    gen.generate("iso_root")

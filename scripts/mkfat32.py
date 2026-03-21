#!/usr/bin/env python3
"""Minimal FAT32 formatter for disk images. Usage: mkfat32.py <image>"""
import struct, sys, os

def mkfat32(disk_path):
    size = os.path.getsize(disk_path)
    BPS  = 512
    SPC  = 8
    RES  = 32
    NFAT = 2
    TOTAL = size // BPS

    # Iteratively compute FAT size
    fat_size = 1
    for _ in range(10):
        data_start = RES + NFAT * fat_size
        total_clusters = (TOTAL - data_start) // SPC
        new_fat = (total_clusters * 4 + BPS - 1) // BPS
        if new_fat == fat_size:
            break
        fat_size = new_fat

    bpb = bytearray(512)
    bpb[0:3] = b'\xeb\x58\x90'
    bpb[3:11] = b'MSDOS5.0'
    struct.pack_into('<H', bpb, 11, BPS)
    bpb[13] = SPC
    struct.pack_into('<H', bpb, 14, RES)
    bpb[16] = NFAT
    struct.pack_into('<H', bpb, 17, 0)
    struct.pack_into('<H', bpb, 19, 0)
    bpb[21] = 0xF8
    struct.pack_into('<H', bpb, 22, 0)
    struct.pack_into('<H', bpb, 24, 63)
    struct.pack_into('<H', bpb, 26, 255)
    struct.pack_into('<I', bpb, 28, 0)
    struct.pack_into('<I', bpb, 32, TOTAL)
    struct.pack_into('<I', bpb, 36, fat_size)
    struct.pack_into('<H', bpb, 40, 0)
    struct.pack_into('<H', bpb, 42, 0)
    struct.pack_into('<I', bpb, 44, 2)  # root_cluster
    struct.pack_into('<H', bpb, 48, 1)
    struct.pack_into('<H', bpb, 50, 6)
    bpb[64] = 0x80
    bpb[66] = 0x29
    struct.pack_into('<I', bpb, 67, 0x12345678)
    bpb[71:82] = b'AUSVERSEOS '
    bpb[82:90] = b'FAT32   '
    bpb[510] = 0x55
    bpb[511] = 0xAA

    with open(disk_path, 'r+b') as f:
        f.seek(0)
        f.write(bpb)
        f.seek(6 * 512)
        f.write(bpb)

        fat = bytearray(fat_size * BPS)
        struct.pack_into('<I', fat, 0, 0x0FFFFFF8)
        struct.pack_into('<I', fat, 4, 0x0FFFFFFF)
        struct.pack_into('<I', fat, 8, 0x0FFFFFFF)

        f.seek(RES * BPS)
        f.write(fat)
        f.seek((RES + fat_size) * BPS)
        f.write(fat)

        root_lba = RES + NFAT * fat_size
        f.seek(root_lba * BPS)
        f.write(bytearray(SPC * BPS))

    print(f"FAT32 formatted: fat_size={fat_size} sectors, total_clusters={total_clusters}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: mkfat32.py <disk.img>")
        sys.exit(1)
    mkfat32(sys.argv[1])

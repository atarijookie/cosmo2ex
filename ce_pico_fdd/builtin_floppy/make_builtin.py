#!/usr/bin/env python3
"""Builds the built-in floppy of ce_pico_fdd - see ../builtin_image.h.

Assembles status.s as the boot sector code and as CE_FDD.TOS (m68k-atari-mint-as / objcopy), puts them on a single sided
80 x 9 FAT12 disk and writes
  ../builtin_floppy_data.h  the non-empty sectors + where the status file is, compiled into the firmware (committed, as
                            Arduino can't run this)
  builtin.st                the same disk with a sample status, to try it in an emulator / check it with mtools

Run it from anywhere after changing status.s or the layout: python3 make_builtin.py
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_HEADER = os.path.join(HERE, '..', 'builtin_floppy_data.h')
OUT_ST = os.path.join(HERE, 'builtin.st')

AS = 'm68k-atari-mint-as'
OBJCOPY = 'm68k-atari-mint-objcopy'

# geometry - single sided, so the whole file system (boot sector, FATs, root dir, status file) is on track 0, side 0
TRACKS = 80
SIDES = 1
SPT = 9
BPS = 512
SPC = 2             # sectors per cluster
RESERVED = 1
FATS = 2
FAT_SECTORS = 2
ROOT_ENTRIES = 16   # one sector
TOTAL = TRACKS * SIDES * SPT

ROOT_SECTOR = RESERVED + FATS * FAT_SECTORS                 # 5
ROOT_SECTORS = ROOT_ENTRIES * 32 // BPS                     # 1
DATA_SECTOR = ROOT_SECTOR + ROOT_SECTORS                    # 6 - cluster 2
CLUSTERS = (TOTAL - DATA_SECTOR) // SPC

STATUS_CLUSTER = 2                                          # 1024 B of text max, sectors 6 and 7 = track 0, sectors 7 + 8
STATUS_TEXT_MAX = SPC * BPS
STATUS_DIR_ENTRY = 1                                        # 0: volume label, 1: status file, 2: CE_FDD.TOS

BOOT_CODE_OFFSET = 0x1e
BOOT_CODE_MAX = 512 - BOOT_CODE_OFFSET - 2                  # checksum word at the end

DOS_TIME = (12 << 11) | (0 << 5)                            # 12:00
DOS_DATE = ((2026 - 1980) << 9) | (11 << 5) | 9             # 2026-11-09


def assemble(boot):
    with tempfile.TemporaryDirectory() as tmp:
        obj = os.path.join(tmp, 'status.o')
        binary = os.path.join(tmp, 'status.bin')
        subprocess.check_call([AS, '-m68000', '--defsym', 'BOOT=%d' % (1 if boot else 0), '-o', obj,
                               os.path.join(HERE, 'status.s')])
        subprocess.check_call([OBJCOPY, '-O', 'binary', '-j', '.text', obj, binary])
        with open(binary, 'rb') as f:
            return f.read()


def boot_sector(code):
    if len(code) > BOOT_CODE_MAX:
        sys.exit('boot code too long: %d B, max %d B' % (len(code), BOOT_CODE_MAX))

    bs = bytearray(BPS)
    bs[0:2] = b'\x60\x1c'                           # bra.s to 0x1e
    bs[2:8] = b'CE2X  '
    bs[8:11] = b'\xce\x2f\xdd'                      # serial number
    struct.pack_into('<HBHBHHBHHHH', bs, 11, BPS, SPC, RESERVED, FATS, ROOT_ENTRIES, TOTAL, 0xf8, FAT_SECTORS,
                     SPT, SIDES, 0)
    bs[BOOT_CODE_OFFSET:BOOT_CODE_OFFSET + len(code)] = code

    # executable: big endian words sum up to 0x1234
    total = sum(struct.unpack('>255H', bs[:510])) & 0xffff
    struct.pack_into('>H', bs, 510, (0x1234 - total) & 0xffff)
    return bs


def tos_program(code):
    # TOS header, text only, no symbols, then 0 = no relocation (the code is position independent)
    return struct.pack('>HIIIIIIH', 0x601a, len(code), 0, 0, 0, 0, 0, 0) + code + b'\0\0\0\0'


def dir_entry(name, ext, attr, cluster, size):
    return struct.pack('<8s3sB10sHHHI', name.ljust(8).encode(), ext.ljust(3).encode(), attr, b'\0' * 10,
                       DOS_TIME, DOS_DATE, cluster, size)


def set_fat(fat, cluster, value):
    off = cluster * 3 // 2
    if cluster & 1:
        fat[off] = (fat[off] & 0x0f) | ((value << 4) & 0xf0)
        fat[off + 1] = (value >> 4) & 0xff
    else:
        fat[off] = value & 0xff
        fat[off + 1] = (fat[off + 1] & 0xf0) | ((value >> 8) & 0x0f)


def build_disk(boot_code, prg):
    disk = bytearray(TOTAL * BPS)
    disk[0:BPS] = boot_sector(boot_code)

    fat = bytearray(FAT_SECTORS * BPS)
    set_fat(fat, 0, 0xff8)
    set_fat(fat, 1, 0xfff)
    set_fat(fat, STATUS_CLUSTER, 0xfff)

    prg_cluster = STATUS_CLUSTER + 1
    prg_clusters = (len(prg) + SPC * BPS - 1) // (SPC * BPS)
    for i in range(prg_clusters):
        set_fat(fat, prg_cluster + i, 0xfff if i == prg_clusters - 1 else prg_cluster + i + 1)

    for i in range(FATS):
        start = (RESERVED + i * FAT_SECTORS) * BPS
        disk[start:start + len(fat)] = fat

    root = dir_entry('COSMO2EX', '', 0x08, 0, 0)
    root += dir_entry('STATUS', 'TXT', 0x00, STATUS_CLUSTER, 0)     # name and size set by the device
    root += dir_entry('CE_FDD', 'TOS', 0x00, prg_cluster, len(prg))
    disk[ROOT_SECTOR * BPS:ROOT_SECTOR * BPS + len(root)] = root

    start = (DATA_SECTOR + (prg_cluster - 2) * SPC) * BPS
    disk[start:start + len(prg)] = prg
    return disk


def write_header(disk, boot_len, prg_len):
    sectors = [i for i in range(TOTAL) if any(disk[i * BPS:(i + 1) * BPS])]

    lines = []
    lines.append('// Generated by builtin_floppy/make_builtin.py - don\'t edit, run the script again.')
    lines.append('// Built-in floppy: boot code %d B, CE_FDD.TOS %d B, %d non-empty sectors.' % (boot_len, prg_len, len(sectors)))
    lines.append('#ifndef __BUILTIN_FLOPPY_DATA_H__')
    lines.append('#define __BUILTIN_FLOPPY_DATA_H__')
    lines.append('')
    lines.append('#include <cstdint>')
    lines.append('')
    lines.append('#define BUILTIN_TRACKS              %d' % TRACKS)
    lines.append('#define BUILTIN_SPT                 %d' % SPT)
    lines.append('#define BUILTIN_ROOT_SECTOR         %d      // logical sector of the root directory' % ROOT_SECTOR)
    lines.append('#define BUILTIN_STATUS_DIR_OFFSET   %d     // offset of the status file entry in it' % (STATUS_DIR_ENTRY * 32))
    lines.append('#define BUILTIN_STATUS_TEXT_SECTOR  %d      // logical sector of the status file data' % (DATA_SECTOR + (STATUS_CLUSTER - 2) * SPC))
    lines.append('#define BUILTIN_STATUS_TEXT_MAX     %d   // its cluster size' % STATUS_TEXT_MAX)
    lines.append('#define BUILTIN_SECTOR_COUNT        %d' % len(sectors))
    lines.append('')
    lines.append('// logical sector numbers (track * %d + sector - 1) of the sectors below, all the others are empty' % SPT)
    lines.append('static const uint16_t builtinSectorNo[BUILTIN_SECTOR_COUNT] = { %s };' % ', '.join(str(s) for s in sectors))
    lines.append('')
    lines.append('static const uint8_t builtinSectorData[BUILTIN_SECTOR_COUNT][512] = {')
    for s in sectors:
        data = disk[s * BPS:(s + 1) * BPS]
        lines.append('    {   // sector %d' % s)
        for row in range(0, BPS, 16):
            lines.append('        ' + ', '.join('0x%02x' % b for b in data[row:row + 16]) + ',')
        lines.append('    },')
    lines.append('};')
    lines.append('')
    lines.append('#endif')

    with open(OUT_HEADER, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    boot_code = assemble(True)
    prg = tos_program(assemble(False))
    disk = build_disk(boot_code, prg)
    write_header(disk, len(boot_code), len(prg))

    # sample status in the .st: file name + size, text
    sample = b'Cosmo2ex floppy - sample status text\r\n'
    entry = ROOT_SECTOR * BPS + STATUS_DIR_ENTRY * 32
    disk[entry:entry + 11] = b'NO_WIFI TXT'
    struct.pack_into('<I', disk, entry + 28, len(sample))
    text = (DATA_SECTOR + (STATUS_CLUSTER - 2) * SPC) * BPS
    disk[text:text + len(sample)] = sample

    with open(OUT_ST, 'wb') as f:
        f.write(disk)

    print('boot code %d / %d B, CE_FDD.TOS %d B -> %s, %s' % (len(boot_code), BOOT_CODE_MAX, len(prg),
                                                           os.path.relpath(OUT_HEADER), os.path.relpath(OUT_ST)))


if __name__ == '__main__':
    main()

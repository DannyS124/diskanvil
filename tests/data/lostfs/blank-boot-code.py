#!/usr/bin/env python3
# Blanks Windows' boot code out of an NTFS image Windows made: the code in the boot sector, the
# loader in the rest of $Boot, and the same in the backup boot sector at the end. It's Microsoft's,
# so it isn't ours to share, and nothing in the tests reads it; the boot sector's numbers stay.
#   blank-boot-code.py volume.img
import sys

path = sys.argv[1]
with open(path, "r+b") as f:
    data = bytearray(f.read())
    if data[3:11] != b"NTFS    ":
        sys.exit(f"{path}: not an NTFS volume")
    sector = int.from_bytes(data[11:13], "little")
    sectors = int.from_bytes(data[0x28:0x30], "little")

    def blank(at):
        data[at + 0x54 : at + 0x1FE] = bytes(0x1FE - 0x54)  # between the numbers and the 55 AA

    blank(0)
    # $Boot is 8 KiB; its last 512 bytes are the shadow copies' header (VSS's, not code), kept.
    data[sector:0x1E00] = bytes(0x1E00 - sector)
    backup = sectors * sector
    if data[backup + 3 : backup + 11] == b"NTFS    ":
        blank(backup)
    f.seek(0)
    f.write(data)

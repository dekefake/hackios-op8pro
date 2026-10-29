#!/usr/bin/env python3
"""Make Resources/DTBs/instantnoodlep.dtb (the OnePlus 8 Pro's device tree for the UEFI) from the phone's own boot partition.

usage: mk-instantnoodlep-dtb.py BOOT.img [OUT.dtb]

BOOT.img is the phone's active-slot boot partition (`adb exec-out "su -c 'dd if=/dev/block/by-name/boot_b'" > boot_b.img`, or see
docs/devices/instantnoodlep.md). It carries ~18 device trees; ABL picks one by msm-id/board id. The ones for this phone are
the three "kona ... SoC 19811" trees. They are written one after the other, followed by the 173-byte OnePlus signature tree
that ends the OnePlus 8's list (files/Resources/DTBs/instantnoodle.dtb), which is how Mu-Silicium's other OnePlus ports are laid out.
"""
import re, struct, sys, pathlib

here = pathlib.Path(__file__).resolve().parent
img = pathlib.Path(sys.argv[1]).read_bytes()
out = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else here / 'files/Resources/DTBs/instantnoodlep.dtb')

blobs = []
for m in re.finditer(b'\xd0\x0d\xfe\xed', img):
    o = m.start()
    if o + 28 > len(img):
        continue
    total, off_struct, off_str, off_rsv, ver, last_comp = struct.unpack('>6I', img[o + 4:o + 28])
    if ver in (16, 17) and last_comp <= ver and 40 <= total <= 4 << 20 and o + total <= len(img) and off_struct < total:
        blobs.append(img[o:o + total])
mine = [b for b in blobs if b'SoC 19811' in b]
if len(mine) != 3:
    sys.exit(f'expected 3 device trees for SoC 19811 in the image, found {len(mine)} (of {len(blobs)} trees)')
tail = (here / 'files/Resources/DTBs/instantnoodle.dtb').read_bytes()[-173:]
out.write_bytes(b''.join(mine) + tail)
print(f'{out}: {len(mine)} trees + signature tree, {out.stat().st_size} bytes')

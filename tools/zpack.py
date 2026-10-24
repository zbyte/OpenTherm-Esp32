"""Firmware package for ThingsBoard: the image zlib-compressed (<image>.zlib).

Firmware from 1.2.12 on unpacks it while downloading (fw_port.c), so a
metered link carries about a third less. Usage: zpack.py <image.bin>
"""

import sys
import zlib

src = sys.argv[1]
with open(src, "rb") as f:
    data = f.read()
packed = zlib.compress(data, 9)
assert zlib.decompress(packed) == data
with open(src + ".zlib", "wb") as f:
    f.write(packed)
print("ThingsBoard package: %s.zlib, %d bytes (image %d)" % (src, len(packed), len(data)))

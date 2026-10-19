"""Print or save one named texture from a PS5 RCO resource file (reads a local copy).

usage: rco_icon.py <file.rco> <texture id> [out file]

An RCO holds a tree of elements (texture, sound, ...) whose attributes are each four
32-bit words: name (offset into the attribute-name strings), kind, and two values. A
texture's "id" attribute (kind 9) points at its entry in the id table (a 32-bit word, then
the name), its "src" attribute (kind 8) gives the offset and size of its file in the data
section. This is how the overlay finds the console's icons by name (see ui/src/rco.cpp);
the script is for looking at them.
"""
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
words = struct.unpack_from("<20I", data, 0)
tree, tree_size = words[2], words[3]
ids, ids_size = words[4], words[5]
names, names_size = words[8], words[9]
blobs = words[18]

name_table = data[names:names + names_size]
id_table = data[ids:ids + ids_size]


def name_offset(text):
    at = name_table.find(text.encode() + b"\0")
    return at if at == 0 or (at > 0 and name_table[at - 1] == 0) else -1


ATTR_ID, ATTR_SRC = name_offset("id"), name_offset("src")
text = id_table.find(b"\0" + sys.argv[2].encode() + b"\0") + 1
if text <= 0:
    sys.exit(f"no id {sys.argv[2]}")
wanted = text - 4

for at in range(tree, tree + tree_size - 16, 4):
    if struct.unpack_from("<4I", data, at) != (ATTR_ID, 9, wanted, 0):
        continue
    for attr in range(at, at + 16 * 8, 16):
        name, kind, offset, size = struct.unpack_from("<4I", data, attr)
        if name == ATTR_SRC and kind == 8:
            blob = data[blobs + offset:blobs + offset + size]
            if len(sys.argv) > 3:
                Path(sys.argv[3]).write_bytes(blob)
            else:
                sys.stdout.write(blob.decode("utf-8", "replace") + "\n")
            sys.exit(0)
    sys.exit("the id has no src")
sys.exit("the id is not used in the tree")

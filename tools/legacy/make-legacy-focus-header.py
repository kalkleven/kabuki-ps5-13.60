"""Record the known v9 worker layout before rebuilding it (GPL-3.0-or-later).

Run once against the original v9 renderer, not from the ordinary build script.
The controller can then migrate only that exact idle worker's property literal.
"""
from pathlib import Path
import hashlib
import shutil
import struct
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
data = source.read_bytes()
expected = "82b0e607d1f50d4654656b6e2ae017e694bdc4e7c0f7319e195860211e0eaf64"
if hashlib.sha256(data).hexdigest() != expected:
    raise ValueError("Only the exact original v9 renderer is supported")
if data[:6] != b"\x7fELF\x02\x01":
    raise ValueError("Expected a little-endian 64-bit worker ELF")
phoff = struct.unpack_from("<Q", data, 32)[0]
phsize, phcount = struct.unpack_from("<HH", data, 54)
loads = []
for i in range(phcount):
    kind, flags, offset, address, _, size, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff+i*phsize)
    if kind == 1:
        loads.append((offset, address, size, flags))

def bytes_at(address, length):
    for offset, base, size, _ in loads:
        if base <= address and address+length <= base+size:
            return data[offset+address-base:offset+address-base+length]
    raise ValueError("Address outside file-backed load segment")

literal = b"NodeFocusable\0"
offset = data.find(literal)
if offset < 0 or data.find(literal, offset+1) >= 0:
    raise ValueError("Expected exactly one NodeFocusable literal")
literal_address = None
for start, base, size, flags in loads:
    if start <= offset and offset+len(literal) <= start+size and flags & 2:
        literal_address = base+offset-start
if literal_address is None:
    raise ValueError("Expected the literal in a writable worker segment")
code = bytes_at(0x90, 512)
shutil.copyfile(source, source.with_name("renderer-v9.elf"))
destination.write_text(
    "/* Known v9 worker; generated from our own GPL-3.0-or-later renderer.\n"
    f" * ELF SHA256 {hashlib.sha256(data).hexdigest()} */\n"
    "#pragma once\n#include <cstdint>\nnamespace home_bridge {\n"
    "constexpr std::uint64_t legacy_hook_offset=0x90, legacy_state_offset=0xc000;\n"
    f"constexpr std::uint64_t legacy_focus_offset=0x{literal_address:x};\n"
    "constexpr unsigned char legacy_hook_code[]={\n"
    + "\n".join("    "+",".join(f"0x{b:02x}" for b in code[i:i+16])+"," for i in range(0,len(code),16))
    + "\n};\n}\n", encoding="utf-8")
destination.write_text(destination.read_text(encoding="utf-8") + chr(10).join([
    "namespace home_bridge {",
    "constexpr std::uint64_t legacy_lookup_slot=0x4228, legacy_parent_slot=0x41f0;",
    "constexpr std::uint64_t legacy_lookup_shim_offset=0x30;",
    "}", ""]), encoding="utf-8")
print(f"Saved {destination}; property VA=0x{literal_address:x}, fingerprint=512 bytes")

"""Embed our small lookup helper (GPL-3.0-or-later)."""
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
if len(data) != 77 or data[-16:] != bytes(16):
    raise ValueError("Unexpected lookup helper size or pointer slots")
rows = ["const unsigned char home_lookup_blob[]={"]
rows += [','.join(str(x) for x in data[i:i+32])+',' for i in range(0,len(data),32)]
rows += ["};", "const unsigned int home_lookup_blob_size="+str(len(data))+";", ""]
Path(sys.argv[2]).write_text(chr(10).join(rows), encoding="utf-8")

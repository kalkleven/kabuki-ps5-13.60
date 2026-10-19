"""Download Kabuki's pinned third-party headers into .deps (ignored by git).

Usage: python3 tools/fetch-deps.py
Each file is checked against the SHA-256 in tools/deps.json; a mismatch is an error.
"""
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parents[1]
deps = json.loads((root / "tools" / "deps.json").read_text())
failed = False
for name, entry in deps.items():
    target = root / ".deps" / name
    if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == entry["sha256"]:
        print(f"ok        {name}")
        continue
    with urllib.request.urlopen(entry["url"], timeout=60) as response:
        data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != entry["sha256"]:
        print(f"MISMATCH  {name}: got {digest}", file=sys.stderr)
        failed = True
        continue
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(f"fetched   {name}")
sys.exit(1 if failed else 0)

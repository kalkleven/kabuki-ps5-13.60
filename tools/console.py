"""Install Kabuki on a USB drive in the console and read its log, over the console's FTP server.

Usage:
  python3 tools/console.py HOST drives               list USB drives and what is on them
  python3 tools/console.py HOST install [--usb DIR] [--elf FILE] [--name NAME]
                                                     put build/kabuki-ps5.elf (or FILE) on the USB drive,
                                                     as kabuki-ps5.elf (or NAME)
  python3 tools/console.py HOST log [--lines N]      print the end of Kabuki's log
  python3 tools/console.py HOST stop                 ask a running Kabuki to stop
  python3 tools/console.py HOST clear-stop           remove the stop request before the next run

HOST is the console's address (or set KABUKI_CONSOLE). The FTP port is 2121 (--port).
The payload only ever goes on USB, never on the console's internal storage. USB mount numbers
can change between boots, so the drive is found by what is on it: the one holding
kabuki-ps5.elf or librespot-ps5.elf, unless --usb names it. The upload is written beside the
old file and renamed into place, then checked by size.
"""
import argparse
import io
import os
import re
import sys
from ftplib import FTP, error_perm, error_reply
from pathlib import Path

REMOTE = "/data/custom-overlays"  # Kabuki's log and stop file (renamed to /data/kabuki later)
PAYLOAD = Path(__file__).resolve().parents[1] / "build" / "kabuki-ps5.elf"
MARKERS = ("kabuki-ps5.elf", "librespot-ps5.elf")


def listing(ftp, directory):
    lines = []
    try:
        ftp.retrlines(f"LIST {directory}", lines.append)
    except (error_perm, error_reply):
        return {}
    entries = {}
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) == 9:
            entries[parts[8]] = int(parts[4]) if parts[4].isdigit() else -1
    return entries


def usb_drives(ftp):
    return {f"/mnt/{name}": listing(ftp, f"/mnt/{name}")
            for name in listing(ftp, "/mnt") if re.fullmatch(r"usb[0-7]", name)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", nargs="?", default=os.environ.get("KABUKI_CONSOLE"))
    parser.add_argument("command", choices=("drives", "install", "log", "stop", "clear-stop"))
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--usb", help="the USB drive to install to, for example /mnt/usb0")
    parser.add_argument("--lines", type=int, default=60)
    parser.add_argument("--elf", type=Path, default=PAYLOAD, help="the file to install")
    parser.add_argument("--name", default="kabuki-ps5.elf", help="its name on the drive")
    args = parser.parse_args()
    if not args.host:
        parser.error("give the console's address, or set KABUKI_CONSOLE")
    ftp = FTP()
    ftp.connect(args.host, args.port, timeout=30)
    ftp.login()

    if args.command == "drives":
        for drive, entries in usb_drives(ftp).items():
            found = [m for m in MARKERS if m in entries]
            print(drive, "holds " + ", ".join(found) if found else "")
    elif args.command == "install":
        data = args.elf.read_bytes()
        if "/" in args.name or not args.name.endswith(".elf"):
            sys.exit("--name is a file name ending in .elf")
        if args.usb:
            drive = args.usb
        else:
            drives = [d for d, entries in usb_drives(ftp).items() if any(m in entries for m in MARKERS)]
            if len(drives) != 1:
                sys.exit(f"Expected one USB drive holding {' or '.join(MARKERS)}, found {drives or 'none'}; use --usb")
            drive = drives[0]
        target = f"{drive}/{args.name}"
        ftp.storbinary(f"STOR {target}.partial", io.BytesIO(data))
        try:
            ftp.delete(target)
        except (error_perm, error_reply):
            pass
        ftp.rename(f"{target}.partial", target)
        size = listing(ftp, drive).get(args.name)
        print(target, len(data), "bytes", "ok" if size == len(data) else f"SIZE MISMATCH ({size})")
        if size != len(data):
            sys.exit(1)
    elif args.command == "log":
        data = io.BytesIO()
        ftp.retrbinary(f"RETR {REMOTE}/probe.log", data.write)
        lines = data.getvalue().decode("utf-8", errors="replace").splitlines()
        print("\n".join(lines[-args.lines:]))
    elif args.command == "stop":
        ftp.storbinary(f"STOR {REMOTE}/stop.txt", io.BytesIO(b"stop\n"))
        print("Stop requested. Run clear-stop before the next launch.")
    elif args.command == "clear-stop":
        try:
            ftp.delete(f"{REMOTE}/stop.txt")
            print("Stop request removed.")
        except (error_perm, error_reply):
            print("No stop request was there.")
    ftp.quit()


if __name__ == "__main__":
    main()

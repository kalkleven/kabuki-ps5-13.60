"""Copy the console's fonts and UI resource files that the preview draws with into assets/system.

usage: python ui/tools/fetch_console_assets.py <console address> [port]

Read-only on the console: files are only downloaded (FTP RETR), never written or deleted there.
The copies are Sony's; assets/system is ignored by git and must never be committed or shipped.
Files already present are skipped.
"""
import sys
from ftplib import FTP
from pathlib import Path

FILES = {
    "fonts": [
        "/preinst/common/font/SST-Light.otf",
        "/preinst/common/font/SST-Roman.otf",
        "/preinst/common/font/SST-Medium.otf",
        "/preinst/common/font/SST-Bold.otf",
    ],
    "rco": [
        "/system_ex/vsh_asset/Sce.PlayStation.PUI_UI3.rco",
        "/system_ex/vsh_asset/ReactNative.Components.CommonAssets.rco",
    ],
}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 2121
    root = Path(__file__).resolve().parents[1] / "assets" / "system"
    ftp = FTP()
    ftp.connect(host, port, timeout=60)
    ftp.login()
    ftp.voidcmd("TYPE I")
    for folder, remotes in FILES.items():
        for remote in remotes:
            directory, name = remote.rsplit("/", 1)
            target = root / folder / name
            if target.exists() and target.stat().st_size > 0:
                print("have", target)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            partial = target.with_name(name + ".partial")
            ftp.cwd(directory)
            with open(partial, "wb") as handle:
                ftp.retrbinary(f"RETR {name}", handle.write)
            partial.replace(target)
            print("got ", target, target.stat().st_size)
    ftp.quit()


main()

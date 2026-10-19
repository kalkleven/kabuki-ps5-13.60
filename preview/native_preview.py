#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The native ShellUI control centre in a browser, for layout and motion work on the PC.

Runs ui/host/native_preview.cpp (the console's own NativeModel and native_layout.hpp over a
recorded widget tree) and draws its widgets with the console's SST fonts and UI3 icons.

  py -3 ui/tools/native_preview.py                 sample player (a small librespot stand-in)
  py -3 preview/native_preview.py --console <console-ip>
                                                   the console's live player.json/library.json
                                                   (read over FTP; buttons are not sent back)
Then open http://127.0.0.1:8765. Keys: Space open/close, arrows, Enter = Cross,
Backspace/Esc = Circle, Q = L1, E = R1, S = Square. A gamepad works too.
"""
import argparse
import codecs
import ftplib
import io
import json
import mimetypes
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = Path(os.environ['KABUKI_PREVIEW_OUT']) if os.environ.get('KABUKI_PREVIEW_OUT') else REPO / 'build' / 'native-preview'
DATA = OUT / 'data'
COVERS = OUT / 'covers'
EXE = OUT / ('native_preview.exe' if os.name == 'nt' else 'native_preview')
FONTS = REPO / 'assets' / 'system' / 'fonts'
RCO = REPO / 'assets' / 'system' / 'rco' / 'Sce.PlayStation.PUI_UI3.rco'  # what cxml://custom_overlay/ reads
ICON_CACHE = OUT / 'icons'
REFS = {'screens': REPO / 'build' / 'screens', 'shots': REPO / 'build' / 'shots'}  # console screenshots
PAGE = Path(__file__).with_name('native_preview.html')
SOURCES = [REPO / 'preview/native_preview.cpp', REPO / 'src/ui/native_layout.hpp', REPO / 'src/ui/native_model.hpp',
           REPO / 'src/ui/player.hpp', REPO / 'src/common/records.hpp', REPO / 'src/common/tiles.hpp',
           REPO / 'src/common/content.hpp', REPO / 'src/daemon/service.hpp', REPO / 'src/daemon/librespot_adapter.hpp',
           REPO / 'src/daemon/broker.hpp', REPO / 'src/ui/plugin_host.hpp', REPO / 'sdk/plugin/kabuki_plugin.h']


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SOURCES):
        return
    OUT.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get('CXX') or ('C:/Program Files/LLVM/bin/clang++.exe' if os.name == 'nt' else 'clang++')
    cmd = [compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-deprecated-literal-operator',
           '-I', str(REPO / 'src/ui'), '-I', str(REPO / 'src/common'), '-I', str(REPO / 'src/daemon'),
           '-I', str(REPO / '.deps/nlohmann/include'), '-Wno-tautological-constant-out-of-range-compare',
           '-I', str(REPO / '.deps/stb'), str(SOURCES[0]), '-o', str(EXE)]
    print('building', EXE.name, flush=True)
    subprocess.run(cmd, check=True)


class Host:
    """The preview process: one input line in, one frame out."""
    def __init__(self):
        self.proc = subprocess.Popen([str(EXE), str(DATA), str(FONTS)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, encoding='utf-8', bufsize=1)
        self.lock = threading.Lock()
        self.now = 0
        self.skins = {}

    def step(self, held, pressed, circle, open_, lines=(), linked=True, service=True, providers=()):
        message = dict(held=held, pressed=pressed, circle=circle, open=open_, lines=list(lines),
                       linked=1 if linked else 0, service=1 if service else 0, providers=list(providers))
        with self.lock:
            self.proc.stdin.write(json.dumps(message) + '\n')
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()
        frame = json.loads(line)
        self.now = frame['now_ms']
        for w in frame['widgets']:  # skin pictures come once; the page asks for them by id
            if 'pixels' in w:
                self.skins[w['id']] = {k: w.pop(k) for k in ('pw', 'ph', 'pixels')}
        return frame


class Providers:
    """The provider broker's port (127.0.0.1:38590) for the preview. The sockets are here; the
    broker itself runs in the host, the console daemon's own code (src/daemon/broker.hpp). Try it
    with tools/mock_provider.py or a payload built against sdk/c/kabuki.h."""
    PORT = 38590

    def __init__(self):
        self.lock = threading.Lock()
        self.events = []  # for the host, in order: {conn, open|data|close}
        self.conns = {}
        self.count = 0
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(('127.0.0.1', self.PORT))
        self.server.listen(8)
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            conn, _ = self.server.accept()
            with self.lock:
                self.count += 1
                number = self.count
                self.conns[number] = conn
                self.events.append({'conn': number, 'open': 1})
            threading.Thread(target=self.read, args=(number, conn), daemon=True).start()

    def read(self, number, conn):
        decoder = codecs.getincrementaldecoder('utf-8')('replace')
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                with self.lock:
                    self.events.append({'conn': number, 'data': decoder.decode(data)})
        except OSError:
            pass
        with self.lock:
            self.events.append({'conn': number, 'close': 1})
            self.conns.pop(number, None)
        conn.close()

    def take(self):
        with self.lock:
            events, self.events = self.events, []
        return events

    def deliver(self, frame):
        """The broker's lines to the providers, and the connections it dropped."""
        for item in frame.get('to_providers', []):
            with self.lock:
                conn = self.conns.get(item['conn'])
            if conn is None:
                continue
            try:
                if 'line' in item:
                    conn.sendall((item['line'] + '\n').encode('utf-8'))
                if item.get('close'):
                    conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def write_json(name, value):
    tmp = DATA / (name + '.tmp')
    tmp.write_text(json.dumps(value), encoding='utf-8')
    os.replace(tmp, DATA / name)


def cover_svg(i, text):
    hue = (i * 47) % 360
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="300" height="300"><defs><linearGradient id="g" x1="0" y1="0" '
            f'x2="1" y2="1"><stop offset="0" stop-color="hsl({hue},55%,55%)"/><stop offset="1" stop-color="hsl({(hue+60)%360},50%,25%)"/>'
            f'</linearGradient></defs><rect width="300" height="300" fill="url(#g)"/><text x="20" y="270" font-family="sans-serif" '
            f'font-size="34" fill="white" opacity="0.85">{text}</text></svg>')


class Sample:
    """A small stand-in for librespot-ps5: speaks its control channel's lines (state, library,
    tracks) and answers its commands with a heard timeline, as librespot-ps5 does."""
    SONGS = [("Time's Arrow", 'The Weakerthans', 'Reconstruction Site', 172000), ('Cahokia', 'Greg Freeman', 'I Looked Out', 201000),
             ('A Very Long Song Title That Has To Scroll Along The Card', 'Some Artist With A Long Name Too', 'Album', 245000),
             ('One Great City!', 'The Weakerthans', 'Reconstruction Site', 162000), ('Plea From A Cat Named Virtute', 'The Weakerthans', 'Reconstruction Site', 230000),
             ('Our Retired Explorer', 'The Weakerthans', 'Reconstruction Site', 214000), ('Uncle Sam', 'Greg Freeman', 'I Looked Out', 188000),
             ('Morning Light', 'Mid Air', 'Waves', 199000), ('Second Song', 'Artist', 'Album', 180000), ('Third Song', 'Artist', 'Album', 181000),
             ('Song 11', 'Artist', 'Album', 182000), ('Song 12', 'Artist', 'Album', 183000)]

    def __init__(self, host):
        self.host = host
        self.timers = []
        COVERS.mkdir(parents=True, exist_ok=True)
        for i, s in enumerate(self.SONGS):
            (COVERS / f'{i}.svg').write_text(cover_svg(i, s[2][:14]), encoding='utf-8')
        self.lists = [('collection', 'spotify:collection', 'Liked Songs', 'Playlist · 412 songs'),
                      ('playlist', 'spotify:playlist:a', 'Daily Mix 1', 'Playlist · Spotify'),
                      ('album', 'spotify:album:b', 'Reconstruction Site', 'Album · The Weakerthans'),
                      ('playlist', 'spotify:playlist:c', 'Road Trip', 'Playlist · You'),
                      ('album', 'spotify:album:d', 'I Looked Out', 'Album · Greg Freeman'),
                      ('playlist', 'spotify:playlist:e', 'Focus', 'Playlist · Spotify'),
                      ('playlist', 'spotify:playlist:f', 'Late Night', 'Playlist · You'),
                      ('album', 'spotify:album:g', 'Waves', 'Album · Mid Air'),
                      ('playlist', 'spotify:playlist:h', 'Workout', 'Playlist · Spotify')]
        self.current = 0
        self.queue = list(range(1, 12))
        self.playing, self.shuffle, self.repeat, self.volume = True, False, 'off', 70
        self.position, self.at = 56000, host.now  # heard position at `at`
        self.pending_until = 0
        self.linked = True
        self.out = []  # lines for the daemon's librespot adapter
        self.write()

    def song(self, i):
        t, a, al, d = self.SONGS[i]
        return {'title': t, 'artist': a, 'album': al, 'duration_ms': d, 'cover': f'/sample/covers/{i}.svg'}

    def heard_now(self, now):
        moved = now - self.at if self.playing and now >= self.at else 0
        return min(self.position + moved, self.SONGS[self.current][3])

    def write(self, change=None):
        now = self.host.now
        state = dict(self.song(self.current), playing=self.playing, shuffle=self.shuffle, repeat=self.repeat, volume=self.volume,
                     connected=True, streaming=True, loading=False)
        state['queue'] = [dict(title=self.SONGS[i][0], artist=self.SONGS[i][1], cover=f'/sample/covers/{i}.svg',
                               uri=f'spotify:track:{i}', index=n) for n, i in enumerate(self.queue)]
        if change:  # (in_ms, position, moving, held): held until heard from there, with the buffering light
            in_ms, position, moving, held = change
            state.update(position_ms=held, moving=False, pending=True,
                         changes=[{'in_ms': in_ms, 'position_ms': position, 'moving': moving}])
        else:
            state.update(position_ms=self.heard_now(now), moving=self.playing, pending=False)
        self.out.append(json.dumps({'state': state, 'v': 1}))

    def later(self, ms, fn):
        self.timers.append((self.host.now + ms, fn))

    def tick(self):
        due = [t for t in self.timers if t[0] <= self.host.now]
        self.timers = [t for t in self.timers if t[0] > self.host.now]
        for _, fn in due:
            fn()

    def start(self, index, position=0, delay=900):
        """A new song (or spot): shown at once, heard after `delay`."""
        now = self.host.now
        held = self.heard_now(now) if index == self.current else 0
        if index != self.current:
            held = 0
        self.current = index
        self.position, self.at = position, now + delay
        self.playing = True
        self.write((delay, position, True, held))

    def take(self):
        out, self.out = self.out, []
        return out

    def command(self, line):
        """One of librespot's commands, as the daemon's adapter sends it."""
        c = json.loads(line)
        cmd, now = c.get('cmd'), self.host.now
        if cmd == 'play_pause':
            spot = self.heard_now(now)
            self.playing = not self.playing
            self.position, self.at = spot, now + 350
            self.write((350, spot, self.playing, spot))
        elif cmd == 'previous':
            self.start(self.current, 0, 600)
        elif cmd == 'next' and self.queue:
            self.start(self.queue.pop(0))
        elif cmd == 'shuffle':
            self.shuffle = bool(c.get('on')); self.write()
        elif cmd == 'repeat':
            self.repeat = c.get('mode', 'off'); self.write()
        elif cmd == 'volume':
            self.volume = int(c.get('value', 0)); self.write()
        elif cmd == 'seek':
            ms = int(c.get('ms', 0))
            self.position, self.at = ms, now + 900
            self.write((900, ms, self.playing, ms))
        elif cmd == 'play_queued':
            i = int(c['uri'].rsplit(':', 1)[1])
            if i in self.queue:
                self.queue = self.queue[self.queue.index(i) + 1:]
            self.start(i, 0, 1300)
        elif cmd == 'library':
            items = [dict(kind=k, uri=u, name=n, subtitle=st, cover=f'/sample/covers/{j % len(self.SONGS)}.svg')
                     for j, (k, u, n, st) in enumerate(self.lists)]
            self.later(900, lambda: self.out.append(json.dumps({'library': {'items': items, 'error': ''}, 'v': 1})))
        elif cmd == 'open_list':
            uri = c['uri']
            items = [dict(uri=f'spotify:track:{i}', title=x[0], artist=x[1]) for i, x in enumerate(self.SONGS)]
            self.later(600, lambda: self.out.append(json.dumps({'tracks': {'uri': uri, 'items': items, 'error': ''}, 'v': 1})))
        elif cmd == 'play':
            self.start(int(c['track'].rsplit(':', 1)[1]), 0, 1300)


class Live:
    """Mirrors the console's tile files (read over FTP), as its daemon writes them. Commands
    aren't sent back."""
    def __init__(self, host, address):
        self.host, self.address = host, address
        self.seen = {}
        threading.Thread(target=self.loop, daemon=True).start()

    def connect(self):
        f = ftplib.FTP()
        f.connect(self.address, 2121, timeout=8)
        f.login()
        return f

    def loop(self):
        ftp = None
        while True:
            try:
                ftp = ftp or self.connect()
                names = ['tiles.rec', 'settings.rec', 'show.rec', 'system.rec']
                names += ['tile-' + line.split('\t')[1] + '.rec' for line in (self.seen.get('tiles.rec') or b'').decode().splitlines()
                          if line.startswith('tile\t')]
                for name in names:
                    buf = io.BytesIO()
                    try:
                        ftp.retrbinary(f'RETR /data/custom-overlays/{name}', buf.write)
                    except ftplib.error_perm:
                        continue
                    data = buf.getvalue()
                    if self.seen.get(name) == data:
                        continue
                    self.seen[name] = data
                    (DATA / name).write_bytes(data)  # times are the console's clock: the bar may jump
            except Exception as e:  # keep trying; the console may be asleep
                print('live:', e, flush=True)
                ftp = None
                time.sleep(2)
            time.sleep(0.7)

    def cover(self, path):
        local = COVERS / path.strip('/').replace('/', '_')
        if not local.exists():
            f = self.connect()
            buf = io.BytesIO()
            f.retrbinary('RETR ' + path, buf.write)
            f.quit()
            local.write_bytes(buf.getvalue())
        return local

    def take(self):
        return []

    def command(self, line):
        pass

    def tick(self):
        pass


ICON_LOCK = threading.Lock()
COMMON_RCO = REPO / 'assets' / 'system' / 'rco' / 'ReactNative.Components.CommonAssets.rco'


def common_asset(name):
    """A picture from the React Native common assets (the console's "no cover" art)."""
    if not name.replace('_', '').replace('-', '').isalnum():
        return None
    out = ICON_CACHE / ('common-' + name + '.png')
    with ICON_LOCK:
        if not out.exists():
            ICON_CACHE.mkdir(parents=True, exist_ok=True)
            subprocess.run([sys.executable, str(REPO / 'preview/rco_icon.py'), str(COMMON_RCO), name, str(out)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return out if out.exists() else None


def icon(name):
    """A console icon by id, from the same RCO the console resolves it in (preview/rco_icon.py)."""
    if not name.replace('_', '').isalnum():
        return None
    with ICON_LOCK:
        for ext in ('.svg', '.png'):
            if (ICON_CACHE / (name + ext)).exists():
                return ICON_CACHE / (name + ext)
        ICON_CACHE.mkdir(parents=True, exist_ok=True)
        tmp = ICON_CACHE / (name + '.tmp')
        subprocess.run([sys.executable, str(REPO / 'preview/rco_icon.py'), str(RCO), name, str(tmp)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not tmp.exists():
            return None
        out = ICON_CACHE / (name + ('.png' if tmp.read_bytes()[:4] == b'\x89PNG' else '.svg'))
        os.replace(tmp, out)
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--console', help='mirror this console\'s player (FTP port 2121)')
    ap.add_argument('--port', type=int, default=8765)
    args = ap.parse_args()
    build()
    DATA.mkdir(parents=True, exist_ok=True)
    for old in DATA.glob('*'):
        if old.suffix in ('.rec', '.json', '.tmp') and old.name != 'settings.json':
            old.unlink(missing_ok=True)
    host = Host()
    host.step(0, 0, 0, 0, service=not args.console)
    providers = None
    if not args.console:
        try:
            providers = Providers()
        except OSError as e:
            print(f'providers: cannot listen on 127.0.0.1:{Providers.PORT} ({e}); is another preview running?', flush=True)
    player = Live(host, args.console) if args.console else Sample(host)
    log = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, body, kind, code=200):
            self.send_response(code)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store' if kind.startswith(('application/json', 'text/html')) else 'max-age=60')
            self.end_headers()
            self.wfile.write(body)

        def file(self, path):
            if not path or not Path(path).is_file():
                return self.send(b'not found', 'text/plain', 404)
            self.send(Path(path).read_bytes(), mimetypes.guess_type(str(path))[0] or 'application/octet-stream')

        def do_POST(self):
            if urllib.parse.urlparse(self.path).path != '/snapshot':
                return self.send(b'not found', 'text/plain', 404)
            body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
            (OUT / 'snapshot.png').write_bytes(body)
            self.send(b'saved', 'text/plain')

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
            if url.path == '/':
                return self.send(PAGE.read_bytes(), 'text/html; charset=utf-8')
            if url.path == '/frame':
                player.tick()
                linked = q.get('linked', '1') == '1'
                if not args.console:
                    player.linked = linked
                frame = host.step(int(q.get('held', 0)), int(q.get('pressed', 0)), int(q.get('circle', 0)), int(q.get('open', 0)),
                                  player.take() if linked else [], linked, not args.console,
                                  providers.take() if providers else [])
                if providers:
                    providers.deliver(frame)
                log.extend(frame['commands'])
                for line in frame['librespot']:
                    player.command(line)
                return self.send(json.dumps(frame).encode(), 'application/json')
            if url.path.startswith('/icon/'):
                return self.file(icon(url.path[6:]))
            if url.path.startswith('/common/'):
                return self.file(common_asset(url.path[8:]))
            if url.path.startswith('/skin/'):
                return self.send(json.dumps(host.skins.get(int(url.path[6:]))).encode(), 'application/json')
            if url.path.startswith('/font/'):
                return self.file(FONTS / Path(url.path[6:]).name)
            if url.path == '/refs':
                names = [f'{k}/{p.name}' for k, d in REFS.items()
                         for p in sorted(d.glob('*'), reverse=True) if p.suffix.lower() in ('.jpg', '.png')]
                return self.send(json.dumps(names).encode(), 'application/json')
            if url.path.startswith('/ref/'):
                folder, _, name = urllib.parse.unquote(url.path[5:]).partition('/')
                return self.file(REFS[folder] / Path(name).name if folder in REFS else None)
            if url.path == '/file':
                path = q.get('path', '')
                if path == '/user/data/custom-overlays/chip.png':
                    return self.file(REPO / 'src/ui/assets/chip.png')
                if path.startswith('/sample/covers/'):
                    return self.file(COVERS / Path(path).name)
                if path.startswith('/data/kabuki/art/'):  # a provider's pictures, kept under build/native-preview/art
                    art = (OUT / 'art' / path[len('/data/kabuki/art/'):]).resolve()
                    return self.file(art if (OUT / 'art').resolve() in art.parents else None)
                if isinstance(player, Live):
                    try:
                        return self.file(player.cover(path))
                    except Exception:
                        pass
                return self.send(b'not found', 'text/plain', 404)
            self.send(b'not found', 'text/plain', 404)

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print(f'native preview: http://127.0.0.1:{args.port}  ({"console " + args.console if args.console else "sample player"})', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()

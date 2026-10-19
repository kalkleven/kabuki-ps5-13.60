#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A mock Kabuki provider for the preview: a player tile (media) and a library tile (list), over
the provider protocol (protocol/PROTOCOL.md).

  py -3 preview/native_preview.py        # the preview listens on 127.0.0.1:38590
  py -3 tools/mock_provider.py           # then this connects and adds its two tiles

It also shows what a provider has to do: say hello, send content, answer actions, and connect
again when Kabuki goes away. The pictures are generated PNGs under build/native-preview/art, which
the preview serves as /data/kabuki/art/mock.
"""
import argparse
import json
import os
import select
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = Path(os.environ['KABUKI_PREVIEW_OUT']) if os.environ.get('KABUKI_PREVIEW_OUT') else REPO / 'build' / 'native-preview'
ART = OUT / 'art' / 'mock'
ART_DIR = '/data/kabuki/art/mock'

SONGS = [('Paper Lanterns', 'The Mock Band', 183000), ('Slow Rooms', 'The Mock Band', 211000),
         ('Harbour Lights', 'Example Artist', 197000), ('Quiet Engine', 'Example Artist', 244000),
         ('Long Way Round', 'Fixture', 168000)]


def png(path, hue):
    """A small gradient PNG, so cards have pictures to show."""
    size = 96
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            t = (x + y) / (2 * size)
            rows += bytes((int(40 + 180 * ((hue * 37 % 255) / 255) * (1 - t)), int(60 + 120 * t), int(200 - 140 * t)))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(bytes(rows))) + chunk(b'IEND', b''))


class Mock:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = b''
        self.index = 0
        self.playing = True
        self.position = 40000
        self.volume = 60
        self.shuffle = False
        self.repeat = 'off'
        self.busy = None

    def send(self, message):
        message['v'] = 1
        self.sock.sendall((json.dumps(message) + '\n').encode())

    def content(self, tile, data):
        self.send({'content': {'tile': tile, 'data': data}})

    def cover(self, i):
        return f'{ART_DIR}/{i}.png'

    def player(self):
        title, artist, duration = SONGS[self.index]
        self.content('MOCKMP', {'title': title, 'subtitle': artist, 'art': self.cover(self.index), 'duration_ms': duration,
                                'position_ms': self.position, 'moving': self.playing, 'playing': self.playing,
                                'volume': self.volume, 'shuffle': self.shuffle, 'repeat': self.repeat,
                                'loading': False, 'pending': False, 'changes': []})

    def library(self):
        rows = [{'key': str(i), 'title': t, 'subtitle': a, 'art': self.cover(i)} for i, (t, a, _) in enumerate(SONGS)]
        self.content('MOCKLS', {'busy': self.busy, 'top': {'title': 'Mock library', 'heading': 'Songs', 'rows': rows}})

    def start(self, i):
        self.index, self.position, self.playing, self.busy = i % len(SONGS), 0, True, None
        self.player()
        self.library()
        self.send({'show': {'tile': 'MOCKMP'}})

    def action(self, a):
        tile, name, key, value = a.get('tile'), a.get('action'), a.get('key'), a.get('value')
        print('action', a, flush=True)
        if tile == 'MOCKMP':
            if name == 'play_pause':
                self.playing = not self.playing
            elif name == 'next':
                return self.start(self.index + 1)
            elif name == 'previous':
                return self.start(self.index - 1)
            elif name == 'seek':
                self.position = int(value)
            elif name == 'volume':
                self.volume = int(value)
            elif name == 'shuffle':
                self.shuffle = bool(value)
            elif name == 'repeat':
                self.repeat = ['off', 'all', 'one'][int(value) % 3]
            self.player()
        elif tile == 'MOCKLS' and name == 'select':
            self.busy = key
            self.library()
            self.start(int(key))

    def run(self):
        self.send({'hello': {'provider': 'mock', 'name': 'Mock provider', 'version': '1', 'art_dirs': [ART_DIR],
                             'tiles': [{'id': 'MOCKMP', 'title': 'Mock Player', 'icon': 'icon:iconid_music', 'card': 'media', 'priority': 200},
                                       {'id': 'MOCKLS', 'title': 'Mock Library', 'icon': 'icon:iconid_list', 'card': 'list', 'priority': 200}]}})
        self.player()
        self.library()
        last = time.time()
        while True:
            ready, _, _ = select.select([self.sock], [], [], 1.0)
            if time.time() - last >= 1.0 and self.playing:  # a position every second, as a real player would send
                last = time.time()
                self.position = min(self.position + 1000, SONGS[self.index][2])
                self.content('MOCKMP', {'position_ms': self.position})
            if not ready:
                continue
            data = self.sock.recv(65536)
            if not data:
                return
            self.buffer += data
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                message = json.loads(line)
                if 'action' in message:
                    self.action(message['action'])
                elif 'error' in message:
                    print('error', message['error'], flush=True)
                elif 'shown' in message:
                    print('shown', message['shown']['tile'], flush=True)
                elif 'welcome' in message:
                    print('welcome from Kabuki', message['welcome']['kabuki'], flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=38590)
    args = ap.parse_args()
    ART.mkdir(parents=True, exist_ok=True)
    for i in range(len(SONGS)):
        png(ART / f'{i}.png', i + 1)
    while True:
        try:
            sock = socket.create_connection(('127.0.0.1', args.port), timeout=3)
            sock.settimeout(None)
        except OSError:
            print('Kabuki is not there; trying again in 2 s', flush=True)
            time.sleep(2)
            continue
        try:
            Mock(sock).run()
        except (OSError, ValueError) as e:
            print('connection lost:', e, flush=True)
        print('disconnected; reconnecting', flush=True)
        time.sleep(2)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)

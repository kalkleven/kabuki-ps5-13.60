# Kabuki provider protocol, version 1

A **provider** is any program on the console that wants tiles in Kabuki's panel. It talks to
Kabuki's daemon over a local socket. A provider only ever sends **data**. Kabuki builds the card
from real widgets inside the system interface, so no provider code runs there and a bad provider
can at worst show a wrong card of its own.

Status: draft for review. Everything here is for protocol version `1`.

## Transport

- TCP on loopback only: `127.0.0.1:38590`.
- Newline-delimited JSON: one JSON object per line, UTF-8, `\n` as the separator.
- Every message carries `"v":1` and exactly one message key (`hello`, `content`, ...).
- A line is at most **64 KiB**. Kabuki buffers at most **256 KiB** per client.
- At most **8** providers are connected at once.

A provider connects, sends `hello` first, and waits for `welcome`. If Kabuki isn't running, the
connection fails; retry every 2 seconds.

## Provider → Kabuki

### `hello`

Must be the first message. Sent once per connection.

```json
{"v":1,"hello":{
  "provider":"example",
  "name":"Example",
  "version":"1.0.0",
  "art_dirs":["/data/kabuki/art/example"],
  "tiles":[
    {"id":"EXAMPL","title":"Counter","icon":"icon:iconid_music","card":"list","priority":500}
  ]}}
```

| Field | Rules |
|---|---|
| `provider` | Lowercase letters, digits and `-`, 1 to 24 characters. Not `kabuki`. Used in logs, and as the owner of the tiles. A second connection using a name that is already connected is refused. |
| `name`, `version` | Display text for logs, up to 40 characters each. Optional. |
| `art_dirs` | Up to 4 absolute folders where this provider keeps pictures (see Art). Optional; the default is the `art_dir` Kabuki gives in `welcome`. |
| `tiles` | 1 to 6 tiles. |

Each tile:

| Field | Rules |
|---|---|
| `id` | Exactly 6 letters or digits, chosen by the developer. It is the tile's permanent name: Kabuki saves the user's arrangement by it. Case doesn't matter; Kabuki stores it uppercase. `KBK...` is reserved for Kabuki. If another connected provider already has the id, the tile is refused and the first provider keeps it. |
| `title` | Up to 40 characters. Shown on the rail and as the card's header. |
| `icon` | `icon:<console icon id>` (for example `icon:iconid_music`) or `file:<png path>` under one of `art_dirs`. Optional; a generic icon is used otherwise. |
| `card` | `media`, `list`, or `plugin` (see Plugin cards). |
| `priority` | 0 to 899. Lower comes first in the default arrangement. Default 500. |

A refused tile doesn't refuse the others. Kabuki answers with `welcome`, then one `error` per
refused tile.

### `content`

Sends the data for a template tile. Kabuki owns the layout, the scrolling and the behaviour.

```json
{"v":1,"content":{"tile":"EXAMPL","data":{ ... }}}
```

- `data` **merges** into what Kabuki already holds for the tile. A field that is left out keeps
  its last value. `null` clears a field. A list of `rows` or `points` is replaced as a whole.
- So an update of the playing position is just `{"position_ms":96000}`.
- The tile must belong to this provider and its `card` must match the data (`media` data for a
  `media` tile).
- Updates are **rate limited to 10 per second per tile**. Kabuki keeps the latest and applies it
  on the next turn, so a burst is not an error: the last one wins.

#### `media` data (a player)

| Field | Type | Meaning |
|---|---|---|
| `title`, `subtitle` | string | The song and the artist. |
| `note` | string | A line of advice under the subtitle. |
| `art` | string | Picture file (see Art). |
| `duration_ms` | integer | Length of the song. |
| `position_ms` | integer | Where the song is *now*, as of this message. Kabuki timestamps it on arrival. |
| `moving` | boolean | Whether the position is advancing (playing) from `position_ms`. Kabuki animates the bar between updates. |
| `changes` | array | Up to 12 known future changes, each `{"in_ms":N,"position_ms":N,"moving":bool}`, `in_ms` counted from now. For example, a pause that was asked for but isn't heard yet. A list replaces the earlier one. A message that leaves `changes` out keeps the earlier ones scheduled (they are dropped once they have happened). |
| `pending` | boolean | Something the user asked for (a seek, a skip) isn't heard yet. Kabuki holds the chosen value on the bar until it hears a position near it or the changes run out. |
| `playing`, `loading` | boolean | State of the transport. `loading` shows the buffering light. |
| `shuffle` | boolean | |
| `repeat` | `"off"`, `"all"` or `"one"` | |
| `volume` | 0 to 100, or `null` | `null` (the default) means no volume control is shown. |

#### `list` data (a list of rows, with one level below it)

| Field | Type | Meaning |
|---|---|---|
| `session` | string | Optional opaque generation. Changing it resets this list’s selection and opened sublist, for example after an account switch. |
| `busy` | string | Key of a row whose action is under way. It shows the buffering light. |
| `top` | level | The list shown first. |
| `sub_key` | string or `null` | Key of the row that is opened. |
| `sub` | level | The opened row's own list. Shown while `sub_key` is set. |

A **level** is:

| Field | Type | Meaning |
|---|---|---|
| `title`, `art` | string | The header's title and picture. Empty: the tile's own. |
| `heading` | string | A heading above the rows. |
| `covers` | boolean | Whether rows have pictures. Default `true`. |
| `loading` | boolean | Still loading: Kabuki shows placeholder rows. A new tile starts out loading; sending `rows` ends it unless the same message says `"loading":true`. |
| `empty` | `{"title":"…","detail":"…"}` | Shown when there are no rows. |
| `rows` | array | Up to **500** rows: `{"key":"…","title":"…","subtitle":"…","art":"…","opens":bool}`. |

A row with `opens` shows its own list when chosen. Kabuki sends the `open` action; the provider
answers with `content` that sets `sub_key` and `sub`. Circle sends `close`; the provider clears
`sub_key`. Kabuki handles the animation either way.

### `show`

Asks Kabuki to bring one of the provider's tiles on screen, for example Now Playing once a chosen
song starts. It takes effect only if the panel is open.

```json
{"v":1,"show":{"tile":"EXAMPL"}}
```

### `status`

A short note about the provider. Kabuki shows it on the System card, which has a line for librespot-ps5 today and will get more as providers need them.

```json
{"v":1,"status":{"note":"Connected"}}
```

`note` is up to 60 characters; `null` clears it. Kabuki clears it when the provider disconnects. Until the System card has a line for every provider, only the provider named `librespot` has its note shown.

## Kabuki → provider

### `welcome`

The answer to `hello`.

```json
{"v":1,"welcome":{
  "kabuki":"0.2.0",
  "versions":[1],
  "art_dir":"/data/custom-overlays/art/example",
  "templates":["media","list","plugin"],
  "limits":{"tiles":6,"rows":500,"text":256,"title":40,"line":65536,"updates_per_second":10}}}
```

`art_dir` is where a provider without its own folders may put pictures. `templates` lists the
card kinds this Kabuki can draw. `limits` are the numbers enforced below.

### `action`

The user did something on one of the provider's tiles.

```json
{"v":1,"action":{"tile":"EXAMPL","action":"select","key":"row-3","value":0}}
```

`key` is present when the action has one. `value` is present for `seek`, `volume`, `shuffle` and `repeat`.

| Card | `action` | `key` / `value` |
|---|---|---|
| `media` | `play_pause`, `previous`, `next` | none |
| `media` | `seek` | `value`: the chosen position in milliseconds |
| `media` | `volume` | `value`: 0 to 100 |
| `media` | `shuffle` | `value`: 1 on, 0 off |
| `media` | `repeat` | `value`: 0 off, 1 all, 2 one |
| `list` | `select` | `key`: the row's key, from whichever level is showing |
| `list` | `open` | `key`: the key of a row with `opens` |
| `list` | `close` | none: Circle was pressed in the opened list |

New actions can be added in version 1; a provider ignores actions it doesn't know. Kabuki plays
the matching sound (select, back, move, toggle) itself. Providers can't play sounds.

### `shown`

A tile's card came on screen. Providers use it to fetch on demand, such as a library.

```json
{"v":1,"shown":{"tile":"EXAMPL"}}
```

### `error`

Something a provider sent was refused.

```json
{"v":1,"error":{"code":"bad_tile_id","message":"id must be 6 letters or digits","tile":"ab"}}
```

`tile` is present when it applies. `message` is for logs. Codes:

| Code | Meaning |
|---|---|
| `bad_json`, `bad_message` | The line isn't JSON, or isn't a message this version knows. |
| `unsupported_version` | `v` isn't one Kabuki speaks. The reply also carries `"versions":[1]`, and Kabuki closes the connection. A provider that needs a newer Kabuki can tell its user. |
| `no_hello` | A message before `hello`. |
| `bad_provider` | `provider` is invalid or already connected. Kabuki closes the connection. |
| `bad_tile_id`, `tile_taken`, `reserved_id` | The tile id is invalid, held by another provider, or starts with `KBK`. |
| `bad_card` | The card kind is unknown, or the data doesn't match the tile's card. |
| `bad_plugin` | The plugin path is invalid. A valid path that fails ELF loading shows a tile error. |
| `not_yours` | The tile belongs to another provider, or isn't registered. |
| `too_long` | A text field, title or row count is over its limit. Kabuki cuts it and still accepts the rest where it can; otherwise it refuses the message. |
| `bad_art` | A picture path is not inside a declared folder. The card shows without it. |

## Art and icons

- A provider keeps its pictures in folders it declares in `art_dirs` (or the folder from
  `welcome`). Each folder is absolute and under `/data/` or `/user/data/`.
- A picture path in `art` is accepted only when it is **inside a declared folder**, has no `..`
  and no symlink on the way, is a JPEG or PNG, and is **2 MiB or less**. Anything else is dropped
  with a `bad_art` error and the card shows without the picture.
- Providers never copy pictures for Kabuki: Kabuki reads them where they are. Keep each folder
  small (a cache of about 1,000 files is fine).
- Icons are `icon:<console icon id>` for the console's own icons, or `file:<png>` under the same
  folders.

## Disconnecting

- When a provider disconnects, Kabuki removes its tiles from the rail. Their saved places stay,
  by id, so they return to the same slot when the provider is back. If the panel was on one of
  them, it moves to the next shown tile.
- Kabuki drops a provider that sends a line over 64 KiB or fills its 256 KiB buffer, sends more
  than 200 lines per second, or sends 20 refused messages within 10 seconds. The provider may
  reconnect, and starts again from `hello`.
- A provider that wants Kabuki's tiles back after Kabuki restarts reconnects and sends `hello`
  again. Nothing is remembered about its content: send `content` for every tile.

## Plugin cards

Register `"card":"plugin","plugin":"/mnt/usbN/card.elf"`. The path must be a plain
absolute ELF filename on a mounted USB. Invalid paths return `bad_plugin`.
A valid path whose ELF cannot be mapped keeps its tile and shows a loading error.

Plugins are freestanding ELF64 x86-64 shared objects, at most 2 MiB, with a
`kabuki_plugin_v1` export matching ABI 1. Imports, TLS, constructors and unsupported
relocations are refused. The daemon maps code without invoking its entry point.
Mapped code remains until reboot; per-provider widgets are removed on disconnect.

Send `{"v":1,"data":{"tile":"EXAMPL","data":{"count":3}}}` or the equivalent
`content` form. The free-form object obeys the normal 64 KiB line and 10 Hz limits.
`api.data()` exposes its current JSON. `api.action(id,value)` produces
`{"v":1,"action":{"tile":"EXAMPL","action":"count","value":3}}`.

D-pad and Cross belong to the focused card; Circle and rail navigation remain
Kabuki's. Callback errors and repeated frames above 4 ms disable the card and show
an error. Plugins are **not sandboxed**: bad native code can crash the home screen.
Prefer media/list templates. See [Writing a plugin](../docs/WRITING-A-PLUGIN.md).

A provider replacing a built-in compatibility adapter may declare
`"native":true` in hello. This is a boolean. For librespot this relinquishes the
legacy adapter's tiles before native registration, preventing two owners.

## Versioning

`v` gates breaking changes. Anything that adds fields, actions or error codes stays version 1, so
providers ignore what they don't know. `welcome.versions` lists the versions this Kabuki speaks.

## A whole session

```
provider → {"v":1,"hello":{"provider":"example","tiles":[{"id":"EXAMPL","title":"Counter","card":"list"}]}}
kabuki   → {"v":1,"welcome":{"kabuki":"0.2.0","versions":[1],"art_dir":"…","templates":["media","list","plugin"],"limits":{…}}}
provider → {"v":1,"content":{"tile":"EXAMPL","data":{"top":{"rows":[{"key":"n","title":"Count: 0"},{"key":"reset","title":"Reset"}]}}}}
kabuki   → {"v":1,"shown":{"tile":"EXAMPL"}}
kabuki   → {"v":1,"action":{"tile":"EXAMPL","action":"select","key":"n"}}
provider → {"v":1,"content":{"tile":"EXAMPL","data":{"top":{"rows":[{"key":"n","title":"Count: 1"},{"key":"reset","title":"Reset"}]}}}}
```

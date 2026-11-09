# OpenNOWLite

A small, separate GeForce NOW client for the Watermelon Wine OS board
(STM32MP25, Weston 13). It opens **its own Wayland window** for the stream,
draws it with GStreamer and the SoC's stateless H.264 decoder, and keeps the
seat's keyboard and pointer as its own input pipeline. No Qt, no compositor
module, no shared binary with OpenNOW.

The upstream OpenNOW streamer (pinned to
`v1.0.4-nightly.1515.1`, `SRCREV b3fe71026d8261d386c6f16fcdeb1596ebb85942`)
is used only to carry input and the already-encoded media to NVIDIA. The
H.264 and Opus packets are tapped out of it and handed to this program's own
pipeline, so the picture is never decoded twice. Sign-in, the catalog and the
CloudMatch consent come from the shipped `opennow-core` binary over its
JSON-lines RPC — OpenNOWLite never signs in itself.

## Status

Verified on the seat at 1920x1080:

- 1080p60 video with correct colour, no stutter, no crash
- Opus audio in sync
- Keyboard and pointer input, including mouse-look that turns **past 180°
  and past a full circle** without lifting the mouse
- Tested titles: **Destiny 2** (`100362311`), **Dark and Darker**
  (`102303211`)

OpenNOWLite installs side by side with OpenNOW and shares no file with it,
but the two **cannot stream at the same time**: the core holds an exclusive
lock on its data directory.

## Requirements

On the seat:

- Weston running with a Wayland socket (this image: `wayland-1` under
  `/home/weston`)
- `/usr/bin/opennow-core` from the shipped OpenNOW `v1.0.4-nightly.1515.1`
- GStreamer 1.0 with `gstreamer1.0-plugins-base`, `-bad`, `gstreamer-app`,
  `gstreamer-video`, `gstreamer-wayland`
- `/usr/lib/opennowlite/libopennowlite_streamer_ffi.so` (installed by the
  `opennowlite` package)
- An **OpenNOW account already signed in on this seat** (see
  [Credentials](#credentials-and-sign-in))

## Install

```sh
dpkg -i opennowlite_1.0.4+nightly.1515.1+git0+b3fe71026d-r2.11_arm64.deb
```

No reboot is needed. The package installs:

| Path | What it is |
|---|---|
| `/usr/bin/opennow-lite` | the client itself |
| `/usr/bin/opennowlite` | wrapper that finds the Wayland socket |
| `/usr/lib/opennowlite/libopennowlite_streamer_ffi.so` | the patched streamer FFI library, dlopened by absolute path |

The optional `opennowlite-games` helper is **not** installed by the deb; copy
it from this repository as shown in [Selecting a game](#selecting-a-game).

The wrapper keeps whatever `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY` the
session provides and only falls back to `/home/weston/wayland-1` when they
are unset, so a shell started from Weston and a systemd unit both work.

## Quick start

```sh
opennowlite --launch-app-id=102303211 --fullscreen
```

That launches Dark and Darker, opens a full-screen Wayland window and streams
it. The terminal only receives log lines; the game is in the window.

To stop the stream, press **Ctrl+Alt+Shift+O** at the seat, or kill the
client process.

## Selecting a game

**There is no in-client game picker and no TUI.** One invocation launches one
game, named by its NVIDIA **variant id** (the number GeForce NOW keys a launch
by — not a Steam app id):

```sh
opennowlite --launch-app-id=<variant id>
```

### List the ids

The repository ships `opennowlite-games`, a small stdlib-only Python helper
that asks the shipped core for the same library the OpenNOW window shows:

```sh
install -m 755 opennowlite-games /usr/bin/opennowlite-games
opennowlite-games
```

Output is one game per line, title then launch id:

```
Destiny 2                                        100362311
Dark and Darker                                  102303211
Mortal Online 2                                  100859611
Portal 2                                         18113211
The First Descendant                             103160544
```

`opennowlite-games --json` prints the core's raw reply instead. The core
holds an exclusive lock on its data directory, so run the helper while no
OpenNOW or OpenNOWLite session is active. `--data-dir DIR` points it at
another copy of the data directory.

### Other ways to find an id

- **OpenNOW diagnostics**: every launch through the OpenNOW window logs
  `[activity] game_launch: title=<name> appId=<id>`. On this image the log is
  `/home/weston/.config/OpenNOW/diagnostics/current.log`.
- **The vendor's catalog** via the core's `catalog.library.list` RPC (that is
  what the helper above does).

### Known ids

| Game | `--launch-app-id` |
|---|---|
| Destiny 2 | `100362311` |
| Dark and Darker | `102303211` |

Ids are account-specific in the sense that a game must be in the account's
library to appear; the numbers themselves are NVIDIA's catalog variant ids.

## Is there a TUI?

No. OpenNOWLite deliberately has **no terminal user interface, no menu and no
game picker**. It is a one-shot CLI client:

- the game is chosen once on the command line;
- the picture is a normal Wayland toplevel that this process owns;
- status and diagnostics go to **stderr** (redirect them to a file if you
  run it from a launcher);
- `--help` prints the full option list;
- **Ctrl+Alt+Shift+O** quits.

If you want a graphical catalog, use **OpenNOW** (`opennow-qt`), which is the
GUI client. OpenNOWLite is the plan-B client that exists to give the stream
its own window and a predictable 60 fps path.

## Credentials and sign-in

**You do not need OpenNOW cookies, a username, a password or a token in
OpenNOWLite.** The client never reads or stores credentials.

It asks the shipped `opennow-core` for three things over JSON-lines RPC:

1. `catalog.game.get` — the game behind the variant id
2. `catalog.launch.inspect` — the vendor's launch decision and account scope
3. `session.create` / `session.poll` — the CloudMatch allocation

The core owns sign-in, the catalog and the CloudMatch consent. It uses the
seat's **existing signed-in OpenNOW account**. The session record with its
tokens lives in:

```
~/.config/OpenNOW/fallback-sessions/<hash>.json
```

That file is read by the core, not by OpenNOWLite. Consequences:

- **Sign in once with OpenNOW** (the Qt client) before using OpenNOWLite. The
  account is then shared.
- If that record is missing or unreadable, the launch dies at
  `catalog.game.get authentication_required` after about 11 s. Put the record
  back or sign in again with OpenNOW.
- `--data-dir DIR` makes the core use another data directory, which is how
  you would point OpenNOWLite at a second account.
- OpenNOW and OpenNOWLite cannot stream at the same time: the core holds an
  exclusive lock on the data directory (`The OpenNOW data directory is
  already in use`).
- Tokens never reach the log: the streamer's own lines redact them
  (`credentials=redacted`).

## Command-line reference

### Session

| Option | Meaning |
|---|---|
| `--launch-app-id=ID` | Variant id to launch. Required unless `--test-pattern` is used. |
| `--data-dir DIR` | Data directory for the shipped core. Default: the seat's own (`~/.config/OpenNOW`). |
| `--poll-attempts=N` | How many times, 2 s apart, to poll an allocation the vendor still reports `preparing`. Default `300` (ten minutes), which covers a free-tier queue. |
| `--media-dead=S` | End the session when the video unit count has not advanced for `S` seconds (`0..900`). Default `0` = off. |

### Window and picture

| Option | Meaning |
|---|---|
| `--fullscreen` | Full-screen toplevel. |
| `--width N`, `--height N` | Window size. Default `1920x1080`; minimum 64x64. |
| `--show-cursor` | Keep Weston's desktop arrow over the window. By default the surface asks for no system cursor, so the only pointer is the one the game draws. |
| `--test-pattern` | Play a test pattern in the same window with no session at all. Useful to prove the window, the decoder path and the input path. |
| `--pipeline DESC` | Use a custom GStreamer pipeline description instead of the built-in one. |

### Input

| Option | Meaning |
|---|---|
| `--mouse-look=P` | How much camera turn one swept pixel is worth, `25..400` percent. Default `100`. Scales every relative-mode distance, so act lists stay at 100. |
| `--mouse-absolute` | Send the pointer's *place* (NVST 5) instead of its *distance* (NVST 7). Only for a game that hit-tests a stated point; the camera will stop at the window edge. |
| `--no-pointer-lock` | Keep the relative distance path but do not lock the seat's pointer to the window (pointer-constraints, oneshot lifetime). |

By default the client takes mouse-look distance from
`zwp_relative_pointer_v1`, whose deltas Weston does not clamp to the window,
which is what lets the camera turn past 180° and past a full circle. It
locks the pointer to its surface while relative mode is on and releases the
lock as soon as the pointer leaves. If the compositor has no relative
pointer, it falls back to the change of the seat's absolute place.

The library also reads `OPENNOW_MOUSE_SENSITIVITY` (`0.1..3.0`) and
`OPENNOW_MOUSE_ACCELERATION` (`1.0..150.0`) from the process environment, but
that happens once at first use; a client execed without those variables
cannot set them later. `--mouse-look` is the equivalent knob that always
works.

### Unattended pilot

These are for automated rounds with nobody at the seat.

| Option | Meaning |
|---|---|
| `--auto-pilot` | Drive the game's own notices automatically. |
| `--pilot-at=LIST` | Comma list of `second:suffix` acts. Default `63:r,212:r`. |
| `--pilot-times=N` | How many keep-alive jiggles after the list. Default `64`. |
| `--pilot-gap=S` | Seconds between keep-alive jiggles. Default `60`. |
| `--pilot-click` | Also click once at `--pilot-point`, 3 s after the last act. |
| `--pilot-point=WxH` | Click point. Default: the middle of the window. |

Act suffixes:

| Suffix | Act |
|---|---|
| *(none)* | Pointer move |
| `e` | Escape |
| `r` | Return |
| `s` | Space |
| `c` | Left click at `--pilot-point` |
| `kN` | Physical evdev key `N` (e.g. `k16` = q, `k57` = Space, `k103` = Up) |
| `mDXxDY` | One mouse-look distance, e.g. `m40x0`, `m-40x0` |

### Capture and diagnostics

| Option | Meaning |
|---|---|
| `--record=FILE` | Write raw H.264 into `FILE`. The stream starts mid-GOP, so frames before the next keyframe are not decodable. |
| `--record-seconds=S` | Length of the recording window. Default `120`. |
| `--record-from=S` | Start the recording window `S` seconds into the session. Default `0`. |
| `--debug-input` | Log every input submit with its session offset and the streamer call's duration. |
| `--debug-audio=FILE` | Write the first Opus frames as length-prefixed packets, for an offline Ogg mux. |
| `--debug-audio-seconds=S` | How many seconds of audio to write. Default `60`. |
| `--help` | Print the full usage text. |

## Troubleshooting

| Message | Cause and fix |
|---|---|
| `opennowlite: --launch-app-id is required` | No game id and no `--test-pattern`. |
| `session_allocation_unresolved` | A stale `unresolved-allocation.json` in the data directory. Move that one file aside (e.g. to `backup/`) and retry. It carries no credentials. |
| `session_conflict` | The vendor still holds the previous instance, typically for 1–2 minutes after a failed or stopped session. Wait and retry. |
| `catalog.game.get authentication_required` | The seat has no usable signed-in session. Sign in with OpenNOW, or restore `fallback-sessions/<hash>.json`. |
| `catalog_game_not_found` | Wrong id: you passed the catalog UUID or a game that is not in this account's library. Use the numeric launch id. |
| `The OpenNOW data directory is already in use` | OpenNOW or another OpenNOWLite session is running. Stop it first. |
| `core process unavailable` | `/usr/bin/opennow-core` is missing or not executable. |
| `pointer locked to the window` never appears | The compositor has no pointer-constraints or relative-pointer support; the client falls back to absolute deltas and the camera stops at the window edge. |
| Media starts but the window stays black | Check that the patched FFI library is at `/usr/lib/opennowlite/`; the client dlopens it by absolute path. |

## Building

The authoritative build is the Yocto recipe in the OS tree:

```
layers/meta-embedded-apps/recipes-apps/opennowlite/opennowlite_git.bb
```

It pins upstream `SRCREV b3fe71026d8261d386c6f16fcdeb1596ebb85942`, applies
`0001-hand-encoded-media-to-the-host.patch`, cross-builds the FFI library for
`aarch64`, and compiles `opennow-lite.c` with the Wayland protocol code
(`xdg-shell`, `relative-pointer-unstable-v1`,
`pointer-constraints-unstable-v1`) generated by `wayland-scanner`.

```sh
bitbake opennowlite
```

This repository mirrors the client source and the patch; the recipe is what
produces the installable deb.

## Layout

| Path | Contents |
|---|---|
| `opennow-lite.c` | the client |
| `opennowlite-games` | library/launch-id helper (this README's game picker) |
| `opennowlite-launch.sh` | source of the `/usr/bin/opennowlite` wrapper |
| `0001-hand-encoded-media-to-the-host.patch` | the encoded-media tap applied to the pinned upstream tree |
| `native/opennow-streamer/` | the pinned upstream streamer tree the FFI library is built from |

## License

MIT, matching the OpenNOW recipe in this image.

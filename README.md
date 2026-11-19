# tn5250 — Fork with CCSID 1145 (Euro) Support

Fork of [tn5250/tn5250](https://github.com/tn5250/tn5250) adding CCSID 1145 (Spain/Latin America + Euro sign) support with wide-character rendering.

## What This Fork Adds

**CCSID 1145 support** — the Euro-enabled variant of CCSID 284 (Spain). The Euro sign (U+20AC) is properly rendered on UTF-8 terminals using ncurses wide-character support (`add_wch`).

The upstream tn5250 only supports CCSID 284 which lacks the Euro sign. CCSID 1145 is identical to 284 except EBCDIC byte 0x9F maps to € instead of ¤.

In this fork `map=1145` is the default for the interactive terminal, so you only need `map=` to pick a different CCSID. The printer tools (`lp5250d`, `scs2*`) still default to 37.

> **Note:** The accented character fix ([setlocale issue](https://github.com/tn5250/tn5250/pull/56)) is now fixed upstream in commit [e1ce065](https://github.com/tn5250/tn5250/commit/e1ce0657105248afce6bf7d1fb7e433861864e1c).

## Pre-built Binary (macOS arm64)

A signed and Apple-notarized binary is available in [Releases](https://github.com/TheBeachLab/tn5250/releases).

- **Signed:** Developer ID Application: Beach Lab LTD
- **Notarized:** Apple notarization accepted (no Gatekeeper warnings)

### Install pre-built binary

```bash
# Install dependencies
brew install ncurses openssl@3 luit

# Download and install
curl -L https://github.com/TheBeachLab/tn5250/releases/latest/download/tn5250-0.19.0-macos-arm64.zip -o /tmp/tn5250.zip
unzip /tmp/tn5250.zip -d /tmp/tn5250-bin
cp /tmp/tn5250-bin/tn5250 ~/.local/bin/tn5250
chmod +x ~/.local/bin/tn5250
```

## Build from Source

```bash
# Dependencies (macOS)
brew install ncurses openssl@3 cmake

# Build
git clone https://github.com/TheBeachLab/tn5250.git
cd tn5250
mkdir build && cd build
cmake .. -DCMAKE_C_FLAGS="-I$(brew --prefix ncurses)/include"
make -j4

# Binary at curses/tn5250
cp curses/tn5250 ~/.local/bin/tn5250
```

## Usage

```bash
tn5250 map=1145 env.TERM=IBM-3179-2 your.as400.host:23
```

For ISO-8859-15 terminal compatibility (e.g. inside tmux), wrap with `luit`:

```bash
luit -encoding ISO-8859-15 tn5250 map=1145 env.TERM=IBM-3179-2 your.as400.host:23
```

### Common EBCDIC code pages

| Code page | Language | Map parameter |
|-----------|----------|---------------|
| 37 | US English | `map=37` |
| 284 | Spain | `map=284` |
| **1145** | **Spain + Euro** | **`map=1145`** |
| 273 | Germany | `map=273` |
| 297 | France | `map=297` |
| 280 | Italy | `map=280` |
| 278 | Sweden/Finland | `map=278` |
| 500 | International | `map=500` |

### Terminal types

| Type | Dimensions | Description |
|------|-----------|-------------|
| IBM-3179-2 | 24x80 | Standard color (default) |
| IBM-3477-FC | 27x132 | Wide mode color |
| IBM-3477-FG | 27x132 | Wide mode monochrome |

## Windows build

The Windows program is the existing GUI (`win32/tn5250-win.c`), a normal window rather than a console. GitHub Actions builds it on every `v*` tag and attaches `tn5250-windows-x64.zip` to that GitHub Release. A tag that contains a hyphen (for example `v0.19.1-windows.1`) is published as a pre-release, so it does not replace the macOS asset on `/releases/latest`.

```text
tn5250 map=1145 env.TERM=IBM-3179-2 host:port
```

On Windows the same options can live in `tn5250rc` next to `tn5250.exe` (`host=`, `map=`, `env.TERM=`). `map` defaults to 1145 and `env.TERM` defaults to `IBM-3179-2`.

The window uses Lucida Console (Consolas, then Courier New, if that face is missing). It is an outline font, so it still scales with the window. Colors follow a 5250 screen: phosphor green, white, amber yellow, turquoise, red, blue, and pink on black. Override any of them with `green=#RRGGBB` and the other color names in `tn5250rc`.

The first window covers about 90% of the work area, so the 24×80 (or 27×132) characters are large. Dragging a border scales the font to fill the window. The grid stays exactly 24×80 or 27×132; only the cells change size. Ctrl and numpad + / − zoom, and Ctrl+0 returns to the large default. The rectangle is stored in `%APPDATA%\tn5250\ui.ini`. `resize_fonts=0` turns this off.

Idle sessions send TCP keepalive probes (60 seconds idle, then every 15 seconds; Windows uses `SIO_KEEPALIVE_VALS` because its default idle time is two hours) and a telnet NOP every 45 seconds. `telnet_keepalive=0` disables only the NOP.

The Windows key map can be overridden from `tn5250rc` with `key.TOKEN=ACTION` lines (for example `key.end=erase_eof`, `key.pause=clear`, `key.shift+insert=dup`). Clear sends the Clear AID, Dup is the 5250 Dup key, and Erase EOF nulls the field from the cursor to its end. `key.erase_input` is available if you map it. Ctrl+Plus, Ctrl+Minus and Ctrl+0 still zoom and are not sent to the host.

`devname_suffix=1`, or a trailing `*` on `env.DEVNAME`, retries a device name that the host rejects as NAME2, NAME3, and so on through NAME9, up to 10 characters. The next name is tried only when the socket drops, or 12 seconds pass, before any 5250 screen. Closing the window does not try another name.

### Cutting a release

Tag the commit that should be built and push the tag:

```bash
git tag v0.19.1-windows.1
git push origin v0.19.1-windows.1
```

The `Windows release` workflow compiles the x64 GUI and `lp5250d.exe` with the static C runtime (OpenSSL is not required for plain telnet) and uploads `tn5250-windows-x64.zip`. Use a hyphenated tag when the macOS zip must remain the asset behind `/releases/latest`. `lp5250d.exe` is the 5250 printer session: `env.IBMMFRTYPMDL` turns on host print transform, `pdf_printer` draws the job on a Windows printer (for example Microsoft Print to PDF), and `pdf_dir` writes a PDF file and opens it. The program stays idle until something starts it.

## License

GPL-2.0 (same as upstream tn5250)

## Credits

- Original project: [tn5250/tn5250](https://github.com/tn5250/tn5250)
- CCSID 1145 support and macOS build: [Beach Lab LTD](https://beachlab.org)

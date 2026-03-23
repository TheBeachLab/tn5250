# tn5250 — Fork with CCSID 1145 (Euro) Support

Fork of [tn5250/tn5250](https://github.com/tn5250/tn5250) adding CCSID 1145 (Spain/Latin America + Euro sign) support with wide-character rendering.

## What This Fork Adds

**CCSID 1145 support** — the Euro-enabled variant of CCSID 284 (Spain). The Euro sign (U+20AC) is properly rendered on UTF-8 terminals using ncurses wide-character support (`add_wch`).

The upstream tn5250 only supports CCSID 284 which lacks the Euro sign. CCSID 1145 is identical to 284 except EBCDIC byte 0x9F maps to € instead of ¤.

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

## License

GPL-2.0 (same as upstream tn5250)

## Credits

- Original project: [tn5250/tn5250](https://github.com/tn5250/tn5250)
- CCSID 1145 support and macOS build: [Beach Lab LTD](https://beachlab.org)

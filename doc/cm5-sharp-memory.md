# Raspberry Pi CM5, Sharp Memory LCD, and Zoom AMS-22

This port keeps the norns drawing API at 128x64 and adds a hardware output
backend named `screen:sharp-memory`. By default the backend maps every Cairo
pixel to an exact 3x3 block, yielding a centered 384x192 image on the 400x240
panel. Its default renderer uses hand-tuned 3x3 patterns to represent all 16
norns levels. Text and stroked paths write into one ordered physical semantic
compositor, so optimized pixels retain the painter order of the public
`screen` API.

The backend speaks the Sharp Memory LCD protocol directly through Linux
`spidev`, with the display's active-high chip select driven by a separate GPIO.
It does not require the `sharp_memory` DRM driver, a framebuffer, a custom
kernel, or a device-tree overlay for the panel. Each frame uses one active-high
CS transaction even though its bytes are written to `spidev` in bounded
fragments. Software VCOM flips every 500 ms (one full polarity cycle per
second) while matron is running.

## Requirements

- Raspberry Pi OS Lite 64-bit on the CM5.
- Adafruit SHARP Memory Display Breakout, 2.7-inch 400x240, product 4694.
- SPI0 enabled and exposed as `/dev/spidev0.0`.
- Zoom AMS-22 connected directly to USB with a data-capable cable.

The stock Raspberry Pi kernel is sufficient. `modinfo sharp_memory` is not a
valid check for this backend because that kernel driver is not used.

## Adafruit display wiring

The Adafruit breakout contains its own regulator, boost converter, logic-level
translation, and the required defaults for software VCOM operation. Connect
only these five pins:

| Adafruit breakout | CM5 40-pin header | 52Pi EP-0129 terminal | Notes |
| --- | --- | --- | --- |
| VIN | pin 1 / 3.3 V | `+3V3` | Do not connect the breakout's `3V3` output |
| GND | pin 6 / GND | `GND` | Common ground |
| CLK | pin 23 / GPIO11 | `SCLK` | SPI0 clock |
| DI / MOSI | pin 19 / GPIO10 | `MOSI` | Display data input |
| CS | pin 22 / GPIO25 | `GPIO25` | Active-high chip select driven separately |

Leave `3V3`, `EXTMODE`, `EXTCOMIN`, and `DISP` on the Adafruit breakout
unconnected. In particular, do not connect the display CS to `CE0`/GPIO8: the
CM5 RP1 SPI driver rejects the `SPI_CS_HIGH` mode required by this display.
Shut down the CM5 and remove power before changing wiring.

## Enable SPI

Make sure `/boot/firmware/config.txt` contains:

```ini
dtparam=spi=on
```

Remove this line if it was added while testing the old framebuffer approach:

```ini
dtoverlay=norns-sharp-ls027b7dh01
```

Reboot:

```sh
sudo reboot
```

After reconnecting, verify the device:

```sh
ls -l /dev/spidev0.0
```

The SPI device normally belongs to the `spi` group, while the GPIO character
device belongs to the `gpio` group. Add the norns user to both, then log out and
back in (or reboot) so the new groups are active:

```sh
sudo usermod -aG spi,gpio "$USER"
```

Use your actual login name instead of `we` if it differs. Confirm access with:

```sh
id
test -r /dev/spidev0.0 -a -w /dev/spidev0.0 && echo "SPI access OK"
ls -l /dev/gpiochip*
```

## Standalone display test

Install the small Python SPI and GPIO bindings and run the included test before
building norns:

```sh
sudo apt install python3-spidev python3-libgpiod gpiod
cd ~/norns
gpiodetect | grep pinctrl-rp1
python3 platform/cm5/test-sharp-memory.py
```

The display should show a checkerboard for ten seconds and then clear. The test
uses GPIO25 as active-high CS plus the same 2 MHz clock, byte ordering,
single-CS fragmented frame transfer, and software VCOM commands as the matron
backend. Do not run it while
matron is using the display.

If the script reports `Permission denied`, refresh the login after adding the
user to the `spi` and `gpio` groups. The script finds the user-facing GPIO chip
by its stable `pinctrl-rp1` name, so a kernel change to its `/dev/gpiochipN`
number does not require a configuration change.

## Building and configuring norns

Install the dependencies from `readme-setup.md`, initialize submodules, and
build a 64-bit Cortex-A76 release:

```sh
git submodule update --init --recursive
python3 platform/cm5/waf-python3.py configure --release --cm5
python3 platform/cm5/waf-python3.py -j4
```

The compatibility launcher is necessary on Debian 13 because its Python no
longer includes APIs used by the repository's bundled Waf 2.0.14. It only
adapts the Waf process; it does not alter the system Python installation.

Copy the CM5 I/O configuration:

```sh
cp ~/norns/platform/cm5/matronrc.lua ~/matronrc.lua
```

Norns derives its runtime tree from the `HOME` environment variable. Run it as
the account that owns the checkout and `dust` directory; for the examples in
this setup that is `me`, so `HOME` must be `/home/me`. Create the writable
runtime directories once:

```sh
mkdir -p ~/dust/code ~/dust/audio/tape ~/dust/data
```

When launching manually, do not use `sudo`: it changes the runtime account and
may change `HOME`. The CM5 systemd units described below explicitly run as
`me` with `HOME=/home/me`.

The default configuration uses `/dev/spidev0.0`, line 25 of the `pinctrl-rp1`
GPIO chip for CS, a 2 MHz SPI clock, and the tested stable rate of 8 frames per
second. A complete frame takes about 50 ms at that clock rate.

`dither='tile3x3'` aligns every pattern with one exact 3x3 logical-pixel tile.
Ten hand-tuned masks provide the integral densities; the fractional density is
distributed across five neighboring tiles, so all 16 norns levels remain
distinct. `tile_density_boost=1` makes intermediate levels slightly denser;
valid values are 0 through 2.

Text is rerasterized into a hard, non-antialiased mask. With
`text_shadow=true`, levels 1 through 7 get one solid physical-pixel shadow at
`+1,+1`; levels 8 through 14 get a second at `+2,+2`; level 15 gets no shadow.
The shadows are undithered background pixels (black in the normal palette and
white when inverted). Change the split with `text_second_shadow_level`.

Stroked paths are flattened and rerendered at the physical 384x192 resolution,
so the same rules apply to horizontal, vertical, diagonal, arc, and Bezier
paths. The procedural outline atlas consists of upper, center, and lower tracks
offset along the local path normal. At the default `screen.line_width(1)`, each
track is one physical Sharp pixel wide, making a fully lit level-15 stroke
three pixels wide. Larger API line widths scale the three tracks together.
The original 128x64 stroke footprint is rasterized as an operation-local mask
and expanded with the same exact 3x3 mapping as the main framebuffer. It first
writes background ownership into the ordered compositor; the physical outline
atlas then writes its foreground tracks on top. A later fill, image, text, or
stroke overwrites those pixels in normal painter order, so neither the
grayscale Cairo stroke nor an older semantic operation can leak through.

Levels 1 through 7 draw the center track as `1/1` through `7/1` dash/space
patterns. These compact physical-pixel periods remain visible even on a short
four-Cairo-pixel indicator. Levels 8 through 12 keep the center solid and add
staggered four-pixel-period side tracks. Levels 13 and 14 keep two tracks solid
while filling the third; level 15 makes all three tracks solid. Filled paths,
images, `poke`, and offscreen images continue through the normal 3x3 tile
renderer. Frames that use a complex Cairo blend operator safely fall back to
that normal framebuffer renderer.

The alternative `dither='edge-aware'` keeps the full Bayer 8x8 pattern in flat
areas and reduces threshold variation near high-contrast edges. Use
`dither='bayer8'` for the original ordered-dither mode. Semantic text and stroke
optimizations are active only in `tile3x3` mode.

`invert=true` selects a paper-like black-on-white UI. With the default
`stretch=false`, every 128x64 Cairo pixel occupies exactly 3x3 physical pixels,
leaving 8-pixel side margins and 24-pixel top and bottom margins.
`stretch=true` instead fills 400x240 at the cost of non-integer scaling and
vertical distortion. Exact physical semantic optimizations are disabled while
stretching; that mode uses the canonical framebuffer renderer.

Software VCOM is maintained by matron even when the image does not change.
Do not leave the display powered for an extended period while matron is stopped.

### Capture the rendered Sharp frame

To diagnose scaling or dithering, request a one-shot dump of the exact 1-bit
400x240 frame prepared for the panel:

```sh
touch /tmp/dump-sharp-frame
```

Cause a screen redraw, for example by turning an encoder. Matron consumes the
trigger and writes the exact output frame plus the renderer's diagnostic
surfaces:

- `/tmp/sharp-frame.pbm`: final 400x240 1-bit panel frame
- `/tmp/sharp-source.png`: canonical 128x64 Cairo framebuffer
- `/tmp/sharp-text.png`: operation-local hard-antialiased text mask
- `/tmp/sharp-stroke.png`: operation-local physical stroke atlas
- `/tmp/sharp-coverage.png`: operation-local logical ownership mask
- `/tmp/sharp-semantics.png`: ordered compositor at physical viewport size

In `sharp-semantics.png`, red-only pixels are canonical Cairo fallback, yellow
is hard text, green is a physical stroke-atlas pixel, and blue is explicit
background ownership (stroke gaps, text antialias suppression, and text
shadows). The full-frame ordered view is authoritative; the other three PNGs
show the most recent operation-local masks used to update it. This makes it
possible to distinguish mask-generation bugs from ordering and final 1-bit
composition bugs.
The trigger is one-shot, so frame capture does not add continuous disk writes.

PBM can be copied directly to another computer or converted to PNG on the CM5:

```sh
sudo apt install imagemagick
magick /tmp/sharp-frame.pbm /tmp/sharp-frame.png
```

The PBM conversion accounts for the panel buffer's opposite bit order and
white-bit polarity, so the file shows the same black and white pixels sent to
the display.

## Zoom AMS-22

The AMS-22 is a USB Audio Class 2.0, two-channel interface with 48 kHz support.
Determine its stable ALSA card ID after connecting it:

```sh
cat /proc/asound/cards
aplay -l
arecord -l
```

Use the identifier inside brackets in `/proc/asound/cards` (or immediately
after `card N:` in `aplay -l`), not the numeric card index.

Copy the JACK configuration, then replace `AMS22` in the environment file if
the identifier reported by ALSA differs:

```sh
sudo install -m 0644 ~/norns/platform/cm5/norns-cm5.env /etc/default/norns-cm5
sudo usermod -aG audio "$USER"
```

The profile uses 48 kHz, 128 frames, and three periods. If USB xruns occur,
increase `-p 128` to `-p 256` in `platform/cm5/systemd/norns-jack.service` and
reinstall the unit. Keep the AMS-22 LOOPBACK switch off to avoid a feedback
path. Use DIRECT MONITOR only when an unprocessed dry signal is desired;
otherwise norns and the interface will monitor the input twice.

## Running under systemd

Stop any manually started `jackd`, `sclang`, or norns processes first so they
do not retain the audio device or websocket ports. Confirm that the required
programs and locally built binaries exist:

```sh
command -v jackd jack_wait sclang
test -x ~/norns/build/ws-wrapper/ws-wrapper && echo "ws-wrapper: OK"
test -x ~/norns/build/norns/norns && echo "norns: OK"
```

Then install the CM5 services:

```sh
sudo install -m 0644 \
  ~/norns/platform/cm5/systemd/norns-jack.service \
  ~/norns/platform/cm5/systemd/norns-sclang.service \
  ~/norns/platform/cm5/systemd/norns-main.service \
  ~/norns/platform/cm5/systemd/norns.target \
  /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now norns.target
```

These units are intentionally configured for the `me` account and the checkout
at `/home/me/norns`. They set `HOME=/home/me`, expose the norns and sclang REPLs
on ports 5555 and 5556, and start components in this order:

```text
JACK -> SuperCollider -> norns
```

Verify all three services:

```sh
systemctl status norns-jack norns-sclang norns-main --no-pager
jack_lsp
systemctl show norns-main -p User -p Group -p Environment
pid=$(pgrep -n norns)
tr '\0' '\n' < "/proc/$pid/environ" | grep -E '^(HOME|USER)='
```

The expected environment is `User=me`, `HOME=/home/me`, and `USER=me`. Follow
the live norns log with:

```sh
journalctl -fu norns-main
```

After rebuilding norns, restart only the main process:

```sh
sudo systemctl restart norns-main
```

To restart the complete audio stack in dependency order:

```sh
sudo systemctl restart norns.target
```

## Controls

The CM5 profile intentionally does not assume a carrier-board pinout for the
three keys and three encoders. Add their evdev paths to `~/matronrc.lua` after
the carrier-board device-tree overlay creates the input devices. Grid, MIDI,
and Maiden can be used while those controls are being brought up.

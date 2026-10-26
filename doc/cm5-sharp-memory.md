# Raspberry Pi CM5, Sharp Memory LCD, and Zoom AMS-22

This port keeps the norns drawing API at 128x64 and adds a hardware output
backend named `screen:sharp-memory`. By default the backend maps every Cairo
pixel to an exact 3x3 block, yielding a centered 384x192 image on the 400x240
panel. It uses deterministic edge-aware 8x8 Bayer dithering to represent all
16 norns levels on a monochrome display while keeping high-contrast contours
clean.

The backend speaks the Sharp Memory LCD protocol directly through Linux
`spidev`, with the display's active-high chip select driven by a separate GPIO.
It does not require the `sharp_memory` DRM driver, a framebuffer, a custom
kernel, or a device-tree overlay for the panel. It also maintains the software
VCOM signal once per second while matron is running.

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
uses GPIO25 as active-high CS plus the same 2 MHz clock, byte ordering, line
chunking, and software VCOM commands as the matron backend. Do not run it while
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
may change `HOME`. For a systemd launch, verify both the configured account and
the process environment:

```sh
systemctl show norns-main -p User -p Group -p Environment
pid=$(pgrep -n norns)
tr '\0' '\n' < "/proc/$pid/environ" | grep -E '^(HOME|USER)='
```

On this CM5 the expected values are `User=me`, `HOME=/home/me`, and `USER=me`.
Stock norns service files use the original `we` account and must not be used
unchanged on an installation owned by `me`.

If `norns-main.service` still contains `we`, create an override with
`sudo systemctl edit norns-main`:

```ini
[Service]
User=me
Group=me
Environment=HOME=/home/me
Environment=USER=me
ExecStart=
ExecStart=/home/me/norns/build/ws-wrapper/ws-wrapper ws://*:5555 /home/me/norns/build/norns/norns
```

Then apply it:

```sh
sudo systemctl daemon-reload
sudo systemctl restart norns-main
```

The default configuration uses `/dev/spidev0.0`, line 25 of the `pinctrl-rp1`
GPIO chip for CS, a 2 MHz SPI clock, and 15 frames per second. A complete frame
takes about 50 ms at that clock rate.
`dither='edge-aware'` keeps the full Bayer pattern in flat and low-contrast
areas, then progressively pulls its thresholds towards 50 percent as local
Cairo-pixel contrast increases. This makes antialiased black/white contours
more solid without adding temporal shimmer. Set `dither='bayer8'` to compare
with the original ordered-dither mode.
`invert=true` selects a paper-like black-on-white UI. With the default
`stretch=false`, every 128x64 Cairo pixel occupies exactly 3x3 physical pixels,
leaving 8-pixel side margins and 24-pixel top and bottom margins.
`stretch=true` instead fills 400x240 at the cost of non-integer scaling and
vertical distortion.

Software VCOM is maintained by matron even when the image does not change.
Do not leave the display powered for an extended period while matron is stopped.

### Capture the rendered Sharp frame

To diagnose scaling or dithering, request a one-shot dump of the exact 1-bit
400x240 frame prepared for the panel:

```sh
touch /tmp/dump-sharp-frame
```

Cause a screen redraw, for example by turning an encoder. Matron consumes the
trigger and writes `/tmp/sharp-frame.pbm`. It logs
`screen:sharp-memory: captured /tmp/sharp-frame.pbm` when the file is complete.
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
sudo mkdir -p /etc/systemd/system/norns-jack.service.d
sudo install -m 0644 \
  ~/norns/platform/cm5/systemd/norns-jack.service.d/10-zoom-ams22.conf \
  /etc/systemd/system/norns-jack.service.d/
sudo usermod -aG audio "$USER"
sudo systemctl daemon-reload
sudo systemctl restart norns-jack
```

Verify JACK before starting all of norns:

```sh
systemctl status norns-jack --no-pager
jack_lsp
```

The profile uses 48 kHz, 128 frames, and three periods. If USB xruns occur,
increase `-p 128` to `-p 256`. Keep the AMS-22 LOOPBACK switch off to avoid a
feedback path. Use DIRECT MONITOR only when an unprocessed dry signal is
desired; otherwise norns and the interface will monitor the input twice.

## Controls

The CM5 profile intentionally does not assume a carrier-board pinout for the
three keys and three encoders. Add their evdev paths to `~/matronrc.lua` after
the carrier-board device-tree overlay creates the input devices. Grid, MIDI,
and Maiden can be used while those controls are being brought up.

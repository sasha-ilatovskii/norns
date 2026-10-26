# Raspberry Pi CM5, Sharp Memory LCD, and Zoom AMS-22

This port keeps the norns drawing API at 128x64 and adds a hardware output
backend named `screen:sharp-memory`. The backend scales that surface to the
panel framebuffer and uses deterministic 8x8 Bayer dithering to represent all
16 norns levels on a monochrome panel.

The included device-tree example targets the common Sharp LS027B7DH01 or
LS027B7DH01A (400x240). For another panel, use its exact model in the overlay;
the upstream Linux driver supports several Sharp Memory LCD models.

## Requirements

- Raspberry Pi OS 64-bit on the CM5.
- A Linux kernel with `CONFIG_TINYDRM_SHARP_MEMORY` (Linux 6.13 or newer has
  the upstream [`sharp_memory` DRM driver](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/tiny/sharp-memory.c))
  and fbdev emulation enabled.
- Sharp Memory LCD connected through SPI0.
- Zoom AMS-22 connected directly to USB with a data-capable cable.

Check the display driver before continuing:

```sh
modinfo sharp_memory
```

If that command cannot find the module, install/build a kernel that enables the
driver before configuring norns.

## LS027B7DH01 wiring and overlay

The sample uses software VCOM and hardware chip select:

| Display | CM5 40-pin header | Notes |
| --- | --- | --- |
| SCLK | pin 23 / GPIO11 | SPI0 clock |
| SI | pin 19 / GPIO10 | SPI0 MOSI |
| SCS | pin 24 / GPIO8 | SPI0 CE0; the panel uses active-high CS |
| DISP | 3.3 V | Display permanently enabled |
| EXTMODE | GND | Enables software VCOM |
| EXTCOMIN | GND | Unused in software VCOM mode |
| VDD, VDDA | 3.3 V | Follow the panel/breakout data sheet |
| VSS, VSSA | GND | Follow the panel/breakout data sheet |

VCOM inversion is mandatory. Leaving `EXTMODE` floating or using the wrong
VCOM mode can permanently damage the panel. If a breakout board already
generates EXTCOMIN, configure the overlay for external VCOM instead.

Compile and install the overlay:

```sh
cd ~/norns
dtc -@ -I dts -O dtb \
  -o /tmp/norns-sharp-ls027b7dh01.dtbo \
  platform/cm5/overlays/norns-sharp-ls027b7dh01-overlay.dts
sudo install -m 0644 /tmp/norns-sharp-ls027b7dh01.dtbo \
  /boot/firmware/overlays/
```

Add these lines to `/boot/firmware/config.txt`, then reboot:

```ini
dtparam=spi=on
dtoverlay=norns-sharp-ls027b7dh01
```

After reboot, find the framebuffer belonging to the panel:

```sh
for name in /sys/class/graphics/fb*/name; do printf '%s: ' "$name"; cat "$name"; done
```

Copy the CM5 I/O configuration and set `dev` to that framebuffer:

```sh
cp ~/norns/platform/cm5/matronrc.lua ~/matronrc.lua
sudo usermod -aG video we
```

The default is 15 fps because a complete 400x240 monochrome frame takes nearly
50 ms at the panel's 2 MHz limit. `invert=true` gives black-on-white, and
`stretch=true` fills 400x240 instead of preserving the 2:1 norns aspect ratio.

## Building for CM5

Install the dependencies from `readme-setup.md`, initialize submodules, and
build a 64-bit Cortex-A76 release:

```sh
git submodule update --init --recursive
./waf configure --release --cm5
./waf -j4
```

The `--cm5` option avoids the ARMv7-only floating-point flags used by older
norns images and enables Cortex-A76 tuning.

## Zoom AMS-22

The [AMS-22 manual](https://zoomcorp.com/documents/2406/E_AMS-22.pdf) specifies
USB Audio Class 2.0, two input/output channels, and 48 kHz support. Determine
the stable ALSA card ID after connecting it:

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
sudo usermod -aG audio we
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

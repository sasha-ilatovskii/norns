-- norns runtime I/O for a CM5 with an Adafruit 2.7-inch 400x240 Sharp Memory
-- Display data connected to SPI0, with its active-high CS on GPIO25. Copy this
-- file to ~/matronrc.lua for the account that runs norns.

_boot.add_io('screen:sharp-memory', {
  dev = '/dev/spidev0.0',

  -- RP1 does not accept SPI_CS_HIGH through spidev. GPIO25 is driven directly
  -- instead; leave CE0/GPIO8 disconnected from the display.
  -- The RP1 chip is selected by its stable kernel name; its gpiochip number
  -- may change between kernel releases.
  cs_chip = 'pinctrl-rp1',
  cs_line = 25,

  -- LS027B7DH01 maximum SPI clock.
  speed_hz = 2000000,

  -- A 400x240 frame takes about 50 ms at the panel's 2 MHz SPI limit. Eight
  -- fps is the tested stable rate and prevents refresh work from accumulating.
  fps = 8,

  -- Hand-tuned 3x3 tiles align exactly with the 3x screen scale and retain all
  -- 16 norns levels by distributing fractional density across five tiles.
  -- Alternatives are 'edge-aware' and 'bayer8'.
  dither = 'tile3x3',
  tile_density_boost = 1,

  -- Text is rerasterized without antialiasing. Levels 1..7 receive a solid
  -- one-pixel diagonal shadow; levels 8..14 receive a second pixel; level 15
  -- needs no shadow because it is already solid.
  text_shadow = true,
  text_second_shadow_level = 8,

  -- false preserves the original white-on-black norns appearance. Set true
  -- for a paper-like black-on-white display.
  invert = false,

  -- false maps every 128x64 Cairo pixel to an exact 3x3 panel block, producing
  -- a centered 384x192 image. Set true to fill the complete 400x240 panel at
  -- the cost of non-integer scaling and vertical distortion.
  stretch = false,
})

-- Add the keys and encoders used by your carrier board here. The paths below
-- are examples; they must match the evdev devices created by your overlay.
-- _boot.add_io('keys:gpio', {dev='/dev/input/by-path/platform-keys-event'})
-- _boot.add_io('enc:gpio',  {dev='/dev/input/by-path/platform-knob1-event', index=1})
-- _boot.add_io('enc:gpio',  {dev='/dev/input/by-path/platform-knob2-event', index=2})
-- _boot.add_io('enc:gpio',  {dev='/dev/input/by-path/platform-knob3-event', index=3})

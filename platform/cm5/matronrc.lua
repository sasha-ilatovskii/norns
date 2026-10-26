-- norns runtime I/O for a CM5 with an Adafruit 2.7-inch 400x240 Sharp Memory
-- Display data connected to SPI0, with its active-high CS on GPIO25. Copy this
-- file to /home/we/matronrc.lua.

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

  -- A 400x240 frame takes about 50 ms at the panel's 2 MHz SPI limit, so
  -- 15 fps leaves some headroom for SPI and scheduler overhead.
  fps = 15,

  -- false preserves the original white-on-black norns appearance. Set true
  -- for a paper-like black-on-white display.
  invert = false,

  -- false preserves the norns 2:1 aspect ratio (400x200 on a 400x240 panel).
  -- Set true to use the complete panel at the cost of vertical distortion.
  stretch = false,
})

-- Add the keys and encoders used by your carrier board here. The paths below
-- are examples; they must match the evdev devices created by your overlay.
-- _boot.add_io('keys:gpio', {dev='/dev/input/by-path/platform-keys-event'})
-- _boot.add_io('enc:gpio',  {dev='/dev/input/by-path/platform-knob1-event', index=1})
-- _boot.add_io('enc:gpio',  {dev='/dev/input/by-path/platform-knob2-event', index=2})
-- _boot.add_io('enc:gpio',  {dev='/dev/input/by-path/platform-knob3-event', index=3})

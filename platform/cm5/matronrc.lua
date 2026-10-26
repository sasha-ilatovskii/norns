-- norns runtime I/O for a CM5 with a Sharp Memory LCD exposed by Linux as a
-- framebuffer. Copy this file to /home/we/matronrc.lua.

_boot.add_io('screen:sharp-memory', {
  -- Check /sys/class/graphics/fb*/name after loading the sharp_memory driver.
  dev = '/dev/fb0',

  -- A 400x240 frame takes about 50 ms at the panel's 2 MHz SPI limit, so
  -- 15 fps leaves some headroom for driver and scheduler overhead.
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

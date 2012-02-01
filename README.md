What is this?
=============

It's a driver for PS/2 keyboards on modern Apple operating systems.

I've taken Apple's open-source driver, and tried to make it useful for
non-Apple keyboards.

Features
========

These can all be configured via the plist.

* Windows and Alt keys are swapped 
* Remap function keys
* Remap capslock to any keycode

Function keys
-------------

Unless you turn this off in the plist, keys are mapped as follows:

* F1: brightness down
* F2: brightness up
* F3: mission control
* F4: dashboard
* F5: F5
* F6: F6
* F7: previous
* F8: play/pause
* F9: next
* F10: mute
* F11: volume down
* F12: volume up

If turned on, 'Insert' and 'Application' (aka 'the right-click-key')
emulate the 'fn' key, so holding either of them down will make the function
keys act as themselves.

Capslock
--------

Any ADB keycode can be used. Here's a few highlights:

* 57: capslock (default)
* 62: right control
* 53: escape

KeyRemap4MacBook has [a more complete
list](https://github.com/tekezo/KeyRemap4MacBook/blob/master/src/core/bridge/keycode/data/KeyCode.data);
ignore anything starting with +VK\_+ though.

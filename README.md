# XPStreamDeck

Standalone X-Plane plugin scaffold for using an Elgato Stream Deck without a separate helper app.

Current state:
- Cross-platform X-Plane plugin shell in C++17.
- Plugin menu, commands, prefs, log file, status window.
- Profile loader for simple key-to-command mappings.
- Native X-Plane command dispatch scaffold for `once` and `hold` actions.
- Native HID backend for Stream Deck MK.2-family 15-key devices.
- Worker-thread key polling with handoff into the X-Plane flight loop.
- Native key-image upload with rendered text labels on the Stream Deck buttons.

## Goals
- No focus dependency on the X-Plane window.
- No external Python or Stream Deck companion process.
- Direct use of `XPLMCommandOnce`, `XPLMCommandBegin`, and `XPLMCommandEnd`.
- Same build pattern as the existing local X-Plane C++ plugins.

## Plugin Identity
- Plugin name: `XPStreamDeck`
- Signature: `com.wahltho.xpstreamdeck`
- Command prefix: `XPStreamDeck`
- Prefs file: `XPStreamDeck.prf`
- Log file: `xpstreamdeck.log`

## Included So Far
- `XPStreamDeck/toggle_window`
- `XPStreamDeck/reload_prefs`
- `XPStreamDeck/test_first_binding`

The test command is still useful without hardware: it executes the first resolved profile binding through the normal dispatch path.

## Current Hardware Scope
- Elgato vendor ID `0x0fd9`
- Stream Deck MK.2 family on the 15-key protocol
- Supported product IDs in the current backend:
  - `0x006d`
  - `0x0080`
  - `0x00a5`
  - `0x00b9`

The plugin opens the deck directly over HID, sets brightness, polls key state, and dispatches X-Plane commands without depending on keyboard focus or a separate helper process.
It also renders simple text labels into JPEG key images and uploads them directly to the deck.

## Profile Format
Profiles live in `<X-Plane>/Resources/plugins/XPStreamDeck/profiles/`.

Current simple syntax:
```ini
# label.<index>=TEXT or TEXT\nTEXT
# key.<index>=<command>|<mode>
label.0=PAUSE
key.0=sim/operation/pause_toggle|once
label.1=FLAPS\nDOWN
key.1=sim/flight_controls/flaps_down|once
label.2=FLAPS\nUP
key.2=sim/flight_controls/flaps_up|once
label.3=BRAKES
key.3=sim/flight_controls/brakes_toggle_max|once
label.4=REV\nHOLD
key.4=sim/engines/thrust_reverse_hold|hold
```

Supported modes:
- `once`
- `hold`

`label.<index>` drives the text rendered onto the corresponding Stream Deck key.
Use `\n` for a manual line break.

## Install Layout
Expected X-Plane layout:
```text
<X-Plane>/Resources/plugins/XPStreamDeck/
  64/
    mac.xpl | lin.xpl | win.xpl
  profiles/
    default.cfg
  log/
```

## Build
See `BUILD.md`.

## Next Steps
- Add state-driven key styles and active/inactive feedback.
- Expand beyond the current MK.2-family 15-key protocol.
- Expand profile format once the device layer exists.

# XPStreamDeck

Standalone X-Plane plugin scaffold for using an Elgato Stream Deck without a separate helper app.

Current state:
- Cross-platform X-Plane plugin shell in C++17.
- Plugin menu, commands, prefs, log file, status window.
- Structured logging with `ERROR`/`WARN`/`INFO`/`DEBUG` levels.
- Profile loader for simple key-to-command mappings.
- Native X-Plane command dispatch scaffold for `once` and `hold` actions.
- Native HID backend for Stream Deck MK.2-family 15-key devices.
- Worker-thread key polling with handoff into the X-Plane flight loop.
- Non-blocking HID polling to avoid stalling the X-Plane main thread.
- Native key-image upload with rendered text labels on the Stream Deck buttons.
- Automatic reconnect after disconnect or late device attach.
- Aircraft-specific profile selection via exact tailnum matching with fallback profile.
- Debug-log toggle via prefs for high-detail field diagnostics.

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

## Logging
- Normal logging writes `ERROR`, `WARN`, and `INFO`.
- `DEBUG` logging is enabled with `debug_logging=1` in `XPStreamDeck.prf`.
- Key-image upload can be disabled with `key_images_enabled=0` while keeping deck input and command dispatch active.
- Logs go to both `XPLMDebugString` and `log/xpstreamdeck.log` when `logfile_enabled=1`.
- The plugin now logs prefs parsing, profile scanning/parsing, tailnum selection, HID connect/reconnect, key-image upload, key events, and command dispatch.

## Prefs Format
Example `XPStreamDeck.prf`:
```ini
enabled=1
logfile_enabled=1
debug_logging=0
key_images_enabled=1
show_window_on_start=0
hid_auto_connect=1
active_profile=default
deck_serial=
brightness=75
```

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
If the deck is unplugged or connected after X-Plane/plugin startup, the plugin retries automatically.

## Profile Format
Profiles live in `<X-Plane>/Resources/plugins/XPStreamDeck/profiles/`.

Current syntax:
```ini
# profile_id=<name>
# tailnum=<exact-tail-number>
# label.<index>=TEXT or TEXT\nTEXT
# key.<index>=<command>|<mode>
profile_id=default
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
`tailnum=` can be repeated and is matched exactly and case-sensitively against `sim/aircraft/view/acf_tailnum`.
If no aircraft-specific profile matches, the plugin falls back to the profile named in `active_profile`.

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
- Expand profile actions beyond `once` and `hold`.

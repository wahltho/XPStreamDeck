# XPStreamDeck

Standalone X-Plane plugin scaffold for using an Elgato Stream Deck without a separate helper app.

Current state:
- Cross-platform X-Plane plugin shell in C++17.
- Plugin menu, commands, prefs, log file, status window.
- Profile loader for simple key-to-command mappings.
- Native X-Plane command dispatch scaffold for `once` and `hold` actions.
- Stream Deck HID backend is not implemented yet.

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

The test command is useful before hardware integration: it executes the first resolved profile binding through the normal dispatch path.

## Profile Format
Profiles live in `<X-Plane>/Resources/plugins/XPStreamDeck/profiles/`.

Current simple syntax:
```ini
# key.<index>=<command>|<mode>
key.0=sim/operation/pause_toggle|once
key.1=sim/flight_controls/flaps_down|once
key.2=sim/flight_controls/flaps_up|once
key.3=sim/flight_controls/brakes_toggle_max|once
key.4=sim/engines/thrust_reverse_hold|hold
```

Supported modes:
- `once`
- `hold`

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
- Add HID backend for Stream Deck MK.2 first.
- Poll key state in a worker thread and queue events into the flight loop.
- Add image and brightness handling.
- Expand profile format once the device layer exists.


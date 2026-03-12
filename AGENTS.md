# AGENTS.md

- Repo: standalone X-Plane plugin scaffold for Elgato Stream Deck integration.
- Primary code is in `src/xpstreamdeck_plugin.cpp`.
- Native HID backend for Stream Deck MK.2-family devices lives in `src/streamdeck_hid.*`.
- Build with CMake; default SDK path in this nested project is `../../SDKs/XPlane_SDK`.
- Current scope: plugin shell plus native HID input, worker-thread key polling, flight-loop event dispatch, prefs, logging, profile parsing, and command dispatch.
- Current hardware target: Stream Deck MK.2-family 15-key devices.

# AGENTS.md

- Repo: standalone X-Plane plugin scaffold for Elgato Stream Deck integration.
- Primary code is in `src/xpstreamdeck_plugin.cpp`.
- Build with CMake; default SDK path in this nested project is `../../SDKs/XPlane_SDK`.
- Current scope: plugin shell with menu, prefs, logging, profile parsing, and command dispatch scaffolding.
- HID/Stream Deck device I/O is intentionally not implemented yet.


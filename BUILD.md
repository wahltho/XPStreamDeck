# Build Instructions

## Requirements
- X-Plane SDK with `CHeaders`
- CMake 3.15+ for manual commands
- CMake 3.21+ if you want to use the checked-in `CMakePresets.json`
- Vendored `hidapi` is included under `external/hidapi`
- Vendored `stb_image_write.h` is included under `external/stb`

This project sits one level deeper than the other local X-Plane repos, so the default SDK path is:
`../../SDKs/XPlane_SDK`

## Build Root

To keep build churn off the iCloud-synced source tree, all build directories now live under:

```text
/Users/wahltho/dev/XPStreamDesk
```

Create it once before the first build:

```bash
mkdir -p /Users/wahltho/dev/XPStreamDesk
```

## Artifacts
- `mac.xpl`
- `lin.xpl`
- `win.xpl`

## macOS

Preferred flow with presets:

```bash
cmake --preset mac-release
cmake --build --preset mac-release

cmake --preset mac-universal-release
cmake --build --preset mac-universal-release
```

Manual equivalent:

```bash
cmake -S . -B "/Users/wahltho/dev/XPStreamDesk/build-mac" \
  -DCMAKE_BUILD_TYPE=Release \
  -DXPLANE_SDK_ROOT="../../SDKs/XPlane_SDK"
cmake --build "/Users/wahltho/dev/XPStreamDesk/build-mac" --config Release

cmake -S . -B "/Users/wahltho/dev/XPStreamDesk/build-mac-universal" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
  -DXPLANE_SDK_ROOT="../../SDKs/XPlane_SDK"
cmake --build "/Users/wahltho/dev/XPStreamDesk/build-mac-universal" --config Release
```

## Linux

Native Linux with presets:

```bash
cmake --preset linux-release
cmake --build --preset linux-release
```

Containerized cross-build from macOS:

```bash
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace \
  -v "/Users/wahltho/dev/XPStreamDesk":/workspace-build \
  -v "$(pwd)/../../SDKs":/SDKs \
  -w /workspace ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y build-essential cmake ninja-build && \
  cmake -S . -B /workspace-build/build-lin -G Ninja -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_ROOT=/SDKs/XPlane_SDK && \
  cmake --build /workspace-build/build-lin"
```

## Windows

Native Visual Studio build to the local build root:

```powershell
cmake -S . -B "$HOME/dev/XPStreamDesk/build-win-vs" -G "Visual Studio 17 2022" -A x64 -DXPLANE_SDK_ROOT=../../SDKs/XPlane_SDK
cmake --build "$HOME/dev/XPStreamDesk/build-win-vs" --config Release
```

Alternative MinGW cross-build from macOS/Linux:

```bash
cmake --preset win-mingw-release
cmake --build --preset win-mingw-release
```

Manual equivalent:

```bash
cmake -S . -B "/Users/wahltho/dev/XPStreamDesk/build-win" -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DXPLANE_SDK_ROOT=../../SDKs/XPlane_SDK
cmake --build "/Users/wahltho/dev/XPStreamDesk/build-win" --config Release
```

## Staging

These copy commands still stage the final plugin binaries into the repo-local `deploy/` tree, but the build trees themselves stay on the local disk:

```bash
mkdir -p deploy/XPStreamDeck/64

cp -f "/Users/wahltho/dev/XPStreamDesk/build-mac-universal/mac.xpl" deploy/XPStreamDeck/64/mac.xpl
cp -f "/Users/wahltho/dev/XPStreamDesk/build-lin/lin.xpl" deploy/XPStreamDeck/64/lin.xpl
cp -f "/Users/wahltho/dev/XPStreamDesk/build-win/win.xpl" deploy/XPStreamDeck/64/win.xpl

# If you built Windows natively with Visual Studio instead of MinGW, use:
# cp -f "$HOME/dev/XPStreamDesk/build-win-vs/Release/win.xpl" deploy/XPStreamDeck/64/win.xpl
```

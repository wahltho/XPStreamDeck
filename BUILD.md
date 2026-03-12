# Build Instructions

## Requirements
- X-Plane SDK with `CHeaders`
- CMake 3.15+

This project sits one level deeper than the other local X-Plane repos, so the default SDK path is:
`../../SDKs/XPlane_SDK`

## Artifacts
- `mac.xpl`
- `lin.xpl`
- `win.xpl`

## macOS
```bash
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_ROOT="../../SDKs/XPlane_SDK"
cmake --build build-mac --config Release

cmake -S . -B build-mac-universal -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DXPLANE_SDK_ROOT="../../SDKs/XPlane_SDK"
cmake --build build-mac-universal --config Release
```

## Linux
```bash
podman run --rm -it --platform=linux/amd64 \
  -v "$(pwd)":/workspace -v "$(pwd)/../../SDKs":/SDKs -w /workspace ubuntu:22.04 bash -lc "\
  apt-get update && apt-get install -y build-essential cmake ninja-build && \
  cmake -S . -B build-lin -G Ninja -DCMAKE_BUILD_TYPE=Release -DXPLANE_SDK_ROOT=/SDKs/XPlane_SDK && \
  cmake --build build-lin"
```

## Windows
```powershell
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 -DXPLANE_SDK_ROOT=../../SDKs/XPlane_SDK
cmake --build build-win --config Release
```

## Staging
```bash
mkdir -p deploy/XPStreamDeck/64

cp -f build-mac-universal/mac.xpl deploy/XPStreamDeck/64/mac.xpl
cp -f build-lin/lin.xpl deploy/XPStreamDeck/64/lin.xpl
cp -f build-win/win.xpl deploy/XPStreamDeck/64/win.xpl
```


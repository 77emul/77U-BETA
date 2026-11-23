# Building 77U for iOS

## Requirements

- A Mac with Xcode 26 or newer
- CMake and Ninja (`brew install cmake ninja nasm pkg-config`)
- An Apple developer account to sign the app for your device

Fetch the pinned dependencies from the source tree:

```bash
./fetch-dependencies.sh
```

cubeb, metal-cpp and xbyak_aarch64 are included in `dependencies/` as source
snapshots. Each keeps its upstream license.

### idevice

The JIT helper uses the idevice C bindings from [StikJIT](https://github.com/StikDebug/StikJIT).
For a local build, use the upstream archive that matches the checked-in header.
Run this from the 77U source directory:

```bash
mkdir -p build-ios/deps
git clone --no-checkout https://github.com/StikDebug/StikJIT.git build-ios/deps/StikJIT
git -C build-ios/deps/StikJIT show 3623e725876f76aecb0520582ad6194bacb15d39:idevice/libidevice_ffi.a \
    > build-ios/deps/libidevice_ffi.a
printf '%s  %s\n' '6524066d54ef23e00d46445c04dfc694e180196d54119d718f68434fa225be35' build-ios/deps/libidevice_ffi.a \
    | shasum -a 256 -c
export IDEVICE_LIBRARY="$PWD/build-ios/deps/libidevice_ffi.a"
```

Release builds use idevice built from source at the revision below. Its
generated header differs from the StikJIT header, but the C functions called
by the bundled Swift code have matching signatures.

```bash
rustup toolchain install 1.94.1 --profile minimal --target aarch64-apple-ios
deps="$PWD/build-ios/deps"
mkdir -p "$deps"
git clone --no-checkout https://github.com/jkcoxson/idevice.git "$deps/idevice"
git -C "$deps/idevice" checkout --detach 902679eb255100b3a367f82ab3d5e85839108592
(
    cd "$deps/idevice"
    export CARGO_HOME="$deps/cargo"
    export CARGO_ENCODED_RUSTFLAGS="--remap-path-prefix=$deps=/build/idevice"
    export BINDGEN_EXTRA_CLANG_ARGS="--sysroot=$(xcrun --sdk iphoneos --show-sdk-path)"
    export IPHONEOS_DEPLOYMENT_TARGET=17.4
    cargo +1.94.1 build --locked --release -p idevice-ffi --target aarch64-apple-ios
)
export IDEVICE_LIBRARY="$deps/idevice/target/aarch64-apple-ios/release/libidevice_ffi.a"
```

Build release dependencies in a neutral path outside a personal home directory.
Set `VCPKG_BINARY_SOURCES=clear` for the configure command below so vcpkg builds
the dependencies there instead of restoring packages with old paths.
The public IPA packer checks the complete app for embedded host paths, including
paths introduced by already compiled vcpkg packages. See the [rustc path mapping
documentation](https://doc.rust-lang.org/rustc/remap-source-paths.html).

## Build

```bash
cmake -S . -B build-ios -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_TOOLCHAIN_FILE=dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-ios-cemu \
    -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets \
    "-DVCPKG_OVERLAY_PORTS=dependencies/vcpkg_overlay_ports_mac;dependencies/vcpkg_overlay_ports" \
    -DCEMUIOS_BUILD_NUMBER=1056 \
    -DCEMUIOS_IDEVICE_LIBRARY="$IDEVICE_LIBRARY" \
    -DCEMUIOS_DEVELOPMENT_TEAM=YOUR_TEAM_ID
xcodebuild -project build-ios/Cemu.xcodeproj -scheme CemuIOS -configuration Release -sdk iphoneos build
```

The first configure builds the dependencies with vcpkg and takes a while.

To make an IPA for sideloading:

```bash
python3 platform/ipados/PackageIPA.py build-ios/platform/ipados/Release-iphoneos/CemuIOS.app 77U.ipa --for-sideloading --work build-ios/ipa
```

## Tests

Most tests in `platform/ipados/Tests` compile a small part of the emulator on
its own and run it on the Mac:

```bash
for test in platform/ipados/Tests/*.py; do
    python3 "$test" || exit 1
done
xcrun clang++ -std=c++20 -Isrc platform/ipados/Tests/MemoryBudgetTests.cpp -o build-ios/memory-budget-tests
build-ios/memory-budget-tests
```

## Optional cover model download

The IPA and source snapshot ship without model weights. CMake sets
`77UCoverModelDownloadURL` in the app's Info.plist to
`https://github.com/77emul/77U-BETA/releases/download/cover-model-v1/Artwork-normal.77umodel` by default.
The payload is published as a release asset on the `cover-model-v1` tag.
To use another HTTPS host, override `CEMUIOS_COVER_MODEL_DOWNLOAD_URL`.
An empty override uses a manual HTTPS link prompt.

The download file contains the five original files concatenated in this
order: `analytics/coremldata.bin`, `coremldata.bin`, `metadata.json`,
`model.mil`, `weights/weight.bin`. It has 1,325,882 bytes and SHA-256
`0e906600b8cfda0209efe05a0a23a5da2b87691cf2aeb26bc3131be1cea0ce5b`.
Retain the attribution and license when distributing this payload.
See `platform/ipados/Resources/Licenses/RealESRGAN-Model-Source.txt`.

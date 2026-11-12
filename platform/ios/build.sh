#!/bin/bash
# Builds SegaRally2.ipa (unsigned, for LiveContainer / AltStore / SideStore) on a Mac
# with Xcode, CMake and Ninja. Needs rom/srally2.zip (your own; never published).
#   platform/ios/build.sh            -> build/SegaRally2.ipa
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
ROM=${SR2_ROM_ZIP:-$ROOT/rom/srally2.zip}
MVK_VERSION=${MVK_VERSION:-v1.4.2}
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "== [1/6] ROM and the recompiled code"
[ -f "$ROM" ] || { echo "No ROM at $ROM"; exit 1; }
python3 tools/romtool.py "$ROM" data
python3 tools/recomp.py data/crom.bin build/gen

echo "== [2/6] Tools for this Mac (glslang, m68kmake)"
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DSR2_HOST_TOOLS_ONLY=ON >/dev/null
cmake --build build/host --target glslang-standalone m68kmake -j "$JOBS"
GLSLANG=$(find "$ROOT/build/host" -type f -name glslang -perm -u+x | head -1)
M68KMAKE=$ROOT/build/host/m68kmake
[ -x "$GLSLANG" ] && [ -x "$M68KMAKE" ] || { echo "host tools missing"; exit 1; }

echo "== [3/6] MoltenVK $MVK_VERSION"
MVK_DIR=$ROOT/build/moltenvk
if [ ! -f "$MVK_DIR/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" ]; then
    mkdir -p "$MVK_DIR"
    curl -sSfL -o "$MVK_DIR/MoltenVK-ios.tar" \
        "https://github.com/KhronosGroup/MoltenVK/releases/download/$MVK_VERSION/MoltenVK-ios.tar"
    tar -xf "$MVK_DIR/MoltenVK-ios.tar" -C "$MVK_DIR"
fi
MVK_LIB=$MVK_DIR/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a

echo "== [4/6] Configure for iOS (arm64)"
cmake -S . -B build/ios -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
    -DSR2_HOST_GLSLANG="$GLSLANG" -DSR2_HOST_M68KMAKE="$M68KMAKE" -DMOLTENVK_LIB="$MVK_LIB" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON

echo "== [5/6] Build"
cmake --build build/ios --target SegaRally2 -j "$JOBS"

echo "== [6/6] Package"
APP=$(find build/ios -maxdepth 2 -type d -name 'SegaRally2.app' | head -1)
[ -d "$APP" ] || { echo "no SegaRally2.app"; exit 1; }
PAY=build/ipa/Payload
rm -rf build/ipa && mkdir -p "$PAY"
cp -R "$APP" "$PAY/"
GAME=$PAY/SegaRally2.app/game
mkdir -p "$GAME/third_party"
cp -R res textures "$GAME/"
cp -R third_party/fonts "$GAME/third_party/"
cp "$ROM" "$GAME/srally2.zip"
# An ad-hoc signature (LiveContainer and the sideloading tools sign it again)
codesign --force --sign - --timestamp=none "$PAY/SegaRally2.app" || true
(cd build/ipa && rm -f ../SegaRally2.ipa && zip -qry ../SegaRally2.ipa Payload)
ls -la build/SegaRally2.ipa
file "$PAY/SegaRally2.app/SegaRally2"

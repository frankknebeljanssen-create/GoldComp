#!/bin/bash
set -e

SOURCE_DIR="$HOME/Dropbox/plugin development/GoldComp"
PLUGIN_DIR="$HOME/Dropbox/plugin sandbox"
BUILD_DIR="$HOME/Downloads/GoldComp-build"

# Find latest zip by version number (highest vXXX)
ZIP=$(ls "$SOURCE_DIR"/GoldComp_v*.zip 2>/dev/null | sort -V | tail -1)
if [ -z "$ZIP" ]; then
    # Fallback: any GoldComp zip
    ZIP=$(ls -t "$SOURCE_DIR"/GoldComp*.zip 2>/dev/null | head -1)
fi
if [ -z "$ZIP" ]; then
    echo "No GoldComp*.zip found in $SOURCE_DIR"
    exit 1
fi
echo "=== Building from: $ZIP ==="

# Clean and unzip to build dir
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
unzip -o "$ZIP" -d "$BUILD_DIR"
cd "$BUILD_DIR/GoldComp"

if [ ! -d "JUCE" ]; then
    echo "Downloading JUCE..."
    git clone --depth 1 https://github.com/juce-framework/JUCE.git
fi

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --parallel 8

mkdir -p "$PLUGIN_DIR"
cp -R GoldComp_artefacts/Release/VST3/GoldComp.vst3 "$PLUGIN_DIR/"
xattr -cr "$PLUGIN_DIR/GoldComp.vst3"
codesign --force --deep --sign - "$PLUGIN_DIR/GoldComp.vst3"

echo ""
echo "=== GoldComp installed to: $PLUGIN_DIR ==="
echo "Built from: $ZIP"
echo "Restart Ableton to load the plugin."

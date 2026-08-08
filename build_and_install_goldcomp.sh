#!/bin/bash
set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
# Build dir and JUCE checkout live outside Dropbox — syncing them causes
# CMake cache conflicts and pointless upload traffic.
BUILD_DIR="$HOME/Library/Caches/GoldComp-build"
JUCE_DIR="$HOME/Library/Caches/GoldComp-juce"

if [ ! -d "$JUCE_DIR" ]; then
    echo "Downloading JUCE to $JUCE_DIR ..."
    git clone --depth 1 https://github.com/juce-framework/JUCE.git "$JUCE_DIR"
fi

cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DJUCE_PATH="$JUCE_DIR"
cmake --build "$BUILD_DIR" --config Release --parallel 8

mkdir -p "$PLUGIN_DIR"
rm -rf "$PLUGIN_DIR/GoldComp.vst3"
cp -R "$BUILD_DIR/GoldComp_artefacts/Release/VST3/GoldComp.vst3" "$PLUGIN_DIR/"
xattr -cr "$PLUGIN_DIR/GoldComp.vst3"
codesign --force --deep --sign - "$PLUGIN_DIR/GoldComp.vst3"

echo ""
echo "=== GoldComp installed to: $PLUGIN_DIR ==="
echo "Rescan or restart Ableton to load the plugin."

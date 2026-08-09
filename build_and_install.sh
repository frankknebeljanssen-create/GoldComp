#!/bin/bash
set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_NAME="SmartComp"

# Local VST3 folder, so Ableton on this machine picks the build up directly.
LOCAL_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
# Dropbox sandbox, so the studio machine sees the same build without copying.
SANDBOX_DIR="$HOME/Dropbox/plugin sandbox"

# Build dir and JUCE checkout stay outside Dropbox: a synced build dir let
# Dropbox restore a stale CMakeCache mid-session and broke configure. Copying
# the finished bundle into Dropbox afterwards is fine — nothing reads it back.
BUILD_DIR="$HOME/Library/Caches/SmartComp-build"
JUCE_DIR="$HOME/Library/Caches/GoldComp-juce"

if [ ! -d "$JUCE_DIR" ]; then
    echo "Downloading JUCE to $JUCE_DIR ..."
    git clone --depth 1 https://github.com/juce-framework/JUCE.git "$JUCE_DIR"
fi

cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DJUCE_PATH="$JUCE_DIR"
cmake --build "$BUILD_DIR" --config Release --parallel 8

BUNDLE="$BUILD_DIR/${PLUGIN_NAME}_artefacts/Release/VST3/${PLUGIN_NAME}.vst3"
if [ ! -d "$BUNDLE" ]; then
    echo "Build produced no bundle at $BUNDLE" >&2
    exit 1
fi

install_to() {
    local dest="$1"
    [ -d "$dest" ] || { echo "skipping $dest (not present)"; return 0; }
    rm -rf "$dest/${PLUGIN_NAME}.vst3"
    cp -R "$BUNDLE" "$dest/"
    xattr -cr "$dest/${PLUGIN_NAME}.vst3"
    codesign --force --deep --sign - "$dest/${PLUGIN_NAME}.vst3" 2>/dev/null
    echo "  -> $dest"
}

mkdir -p "$LOCAL_DIR"
echo "Installing ${PLUGIN_NAME}.vst3:"
install_to "$LOCAL_DIR"
install_to "$SANDBOX_DIR"

echo ""
echo "Rescan or restart Ableton to load the plugin."

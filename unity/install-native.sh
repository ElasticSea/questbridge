#!/bin/sh
# macOS: build libquestlink.dylib (Release, arm64, libusb linked statically) and install it as
# the Unity package's macOS native plugin:
# com.elasticsea.questlink/Runtime/Plugins/macOS/libquestlink.dylib (git-ignored).
# Usage: unity/install-native.sh [--no-build]
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$here")
if [ "$1" != "--no-build" ]; then
    [ -x /opt/homebrew/bin/brew ] && eval "$(/opt/homebrew/bin/brew shellenv)"
    cmake -S "$repo/native" -B "$repo/native/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$repo/native/build" -j "$(sysctl -n hw.ncpu)" --target questlink
fi
lib="$repo/native/build/bin/libquestlink.dylib"
[ -f "$lib" ] || { echo "$lib not found; build native/ first" >&2; exit 1; }
if otool -L "$lib" | grep -q /opt/homebrew; then
    echo "$lib links Homebrew libraries; players on other Macs would not load it" >&2
    exit 1
fi

dest="$here/com.elasticsea.questlink/Runtime/Plugins/macOS"
mkdir -p "$dest"
cp "$lib" "$dest/libquestlink.dylib"

# Import settings: Editor and Standalone, macOS arm64 only. Fixed GUID so reinstalls keep references.
cat > "$dest/libquestlink.dylib.meta" <<'EOF'
fileFormatVersion: 2
guid: 3d7e5a1c8b2f4c6e9a0d1f2e3b4c5a69
PluginImporter:
  externalObjects: {}
  serializedVersion: 3
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 1
  isOverridable: 1
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
    Any:
      enabled: 0
      settings: {}
    Editor:
      enabled: 1
      settings:
        CPU: ARM64
        DefaultValueInitialized: true
        OS: OSX
    OSXUniversal:
      enabled: 1
      settings:
        CPU: ARM64
  userData:
  assetBundleName:
  assetBundleVariant:
EOF
echo "Installed $dest/libquestlink.dylib"

#!/usr/bin/env bash
# Fetch D7VK (DXVK-Sarek): a native Direct3D 7 -> Vulkan implementation.
#
# Why this and not dgVoodoo: dgVoodoo translates D3D7 -> D3D11 and then needs
# DXVK + MoltenVK under it. Upstream D7VK targets Vulkan 1.4, which MoltenVK
# cannot provide, but DXVK-Sarek backports it to a Vulkan 1.1/1.2 baseline --
# exactly MoltenVK's level -- and ships a 32-bit ddraw.dll, which is what
# Empire Earth needs. It exposes a real "D7VK T&L HAL" device.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

VERSION="1.13.0"
URL="https://github.com/pythonlover02/dxvk-sarek/releases/download/v${VERSION}/dxvk-sarek-${VERSION}.tar.gz"
dest="$SUPPORT_DIR/patches/d7vk"
payload="$dest/dxvk-sarek-${VERSION}/build/x32/ddraw.dll"

if [[ -f "$payload" ]]; then
  log "D7VK ${VERSION} already installed"
  exit 0
fi

mkdir -p "$dest"
log "Downloading DXVK-Sarek ${VERSION} (D7VK)"
if ! curl -fsSL -o "$dest/dxvk-sarek-${VERSION}.tar.gz" "$URL"; then
  die "Could not download DXVK-Sarek from $URL"
fi
tar xf "$dest/dxvk-sarek-${VERSION}.tar.gz" -C "$dest"

[[ -f "$payload" ]] || die "D7VK archive did not contain build/x32/ddraw.dll"
if ! file "$payload" | grep -q "80386"; then
  die "D7VK ddraw.dll is not 32-bit; Empire Earth needs the x32 build"
fi
log "Installed D7VK ${VERSION} into $dest"

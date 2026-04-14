#!/bin/bash
#
# Waifuland Install Script
# Checks dependencies, downloads third-party libraries, and builds the project.
#

set -euo pipefail

# ─── Colors & Helpers ─────────────────────────────────────────────────────────

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
RESET='\033[0m'

info()    { echo -e "${BLUE}[INFO]${RESET}  $*"; }
success() { echo -e "${GREEN}[OK]${RESET}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
error()   { echo -e "${RED}[ERROR]${RESET} $*"; }
step()    { echo -e "\n${BOLD}===> $*${RESET}"; }

# Track overall status
ERRORS=0

# ─── Project Root ─────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SDK_DIR="CubismSdkForNative-5-r.5"
NPROC=$(nproc 2>/dev/null || echo 4)

# ─── Step 1: Check system dependencies ───────────────────────────────────────

step "Checking system dependencies"

check_command() {
    local cmd="$1"
    local pkg_hint="${2:-$1}"
    if command -v "$cmd" &>/dev/null; then
        success "$cmd found: $(command -v "$cmd")"
    else
        error "$cmd not found. Install it (e.g. $pkg_hint)"
        ERRORS=$((ERRORS + 1))
    fi
}

check_pkg_config() {
    local lib="$1"
    local pkg_hint="${2:-$1}"
    if pkg-config --exists "$lib" 2>/dev/null; then
        success "$lib found ($(pkg-config --modversion "$lib" 2>/dev/null || echo 'version unknown'))"
    else
        error "$lib not found via pkg-config. Install it (e.g. $pkg_hint)"
        ERRORS=$((ERRORS + 1))
    fi
}

check_command cmake        "cmake"
check_command make         "make / build-essential"
check_command g++          "g++ / gcc / build-essential"
check_command pkg-config   "pkg-config / pkgconf"
check_command curl         "curl"
check_command unzip        "unzip"

# wayland-scanner
if command -v wayland-scanner &>/dev/null; then
    success "wayland-scanner found: $(command -v wayland-scanner)"
elif pkg-config --exists wayland-scanner 2>/dev/null; then
    SCANNER=$(pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || true)
    if [ -n "$SCANNER" ] && [ -x "$SCANNER" ]; then
        success "wayland-scanner found via pkg-config: $SCANNER"
    else
        error "wayland-scanner not found. Install wayland-devel / wayland"
        ERRORS=$((ERRORS + 1))
    fi
else
    error "wayland-scanner not found. Install wayland-devel / wayland"
    ERRORS=$((ERRORS + 1))
fi

# Libraries via pkg-config
check_pkg_config wayland-client  "libwayland-dev / wayland-devel"
check_pkg_config wayland-egl     "libwayland-dev / wayland-devel"
check_pkg_config wayland-cursor  "libwayland-dev / wayland-devel"
check_pkg_config egl             "libegl-dev / mesa-libEGL-devel"

if [ "$ERRORS" -gt 0 ]; then
    echo ""
    error "$ERRORS missing dependency(ies). Please install them and re-run this script."
    echo ""
    info  "Arch Linux:       sudo pacman -S --needed base-devel cmake pkgconf wayland wayland-protocols libglvnd egl-wayland curl unzip"
    info  "Ubuntu / Debian:  sudo apt install build-essential cmake pkg-config libwayland-dev wayland-protocols libegl-dev libgl-dev curl unzip"
    info  "Fedora:           sudo dnf install gcc-c++ cmake pkgconf-pkg-config wayland-devel wayland-protocols-devel mesa-libEGL-devel mesa-libGL-devel curl unzip"
    exit 1
fi

# ─── Step 2: Check Live2D Cubism SDK ─────────────────────────────────────────

step "Checking Live2D Cubism SDK"

if [ -d "$SDK_DIR" ] && [ -f "$SDK_DIR/Core/include/Live2DCubismCore.h" ]; then
    success "Cubism SDK found at ./$SDK_DIR"
else
    warn "Cubism SDK not found at ./$SDK_DIR"
    echo ""
    info "To automatically download the Live2D Cubism SDK, you must agree to the following licenses:"
    info "1. https://www.live2d.com/eula/live2d-proprietary-software-license-agreement_en.html"
    info "2. https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html"
    echo ""
    read -p "Do you agree to the Live2D Software License Agreements? [y/N] " CONSENT
    if [[ "$CONSENT" =~ ^[Yy]$ ]]; then
        info "Downloading Live2D Cubism SDK for Native 5-r.5..."
        SDK_URL="https://cubism.live2d.com/sdk-native/bin/CubismSdkForNative-5-r.5.zip?event=cubism_sdk_download&sdk_type=Native&user_status=update&user_type=&version=5-r.5&lang=en"
        if curl -fsSL -o "CubismSdkForNative-5-r.5.zip" "$SDK_URL"; then
            success "SDK downloaded"
            info "Extracting SDK..."
            unzip -oq "CubismSdkForNative-5-r.5.zip" -d "."
            rm -f "CubismSdkForNative-5-r.5.zip"
            if [ -d "$SDK_DIR" ] && [ -f "$SDK_DIR/Core/include/Live2DCubismCore.h" ]; then
                success "Cubism SDK successfully installed to ./$SDK_DIR"
            else
                error "SDK extraction failed or structure is incorrect."
                exit 1
            fi
        else
            error "Failed to download Cubism SDK. Please check your internet connection or download manually."
            exit 1
        fi
    else
        error "License agreement not accepted."
        echo ""
        info  "Please download the Live2D Cubism SDK for Native manually from:"
        info  "  https://www.live2d.com/en/sdk/about/"
        info  ""
        info  "Extract it to this directory so the structure looks like:"
        info  "  $(pwd)/$SDK_DIR/"
        exit 1
    fi
fi

# ─── Step 3: Download third-party dependencies (GLEW & GLFW) ─────────────────

step "Setting up third-party dependencies (GLEW & GLFW)"

GLEW_VERSION=2.3.1
GLFW_VERSION=3.4
THIRDPARTY_DIR="$SCRIPT_DIR/thirdParty"

# GLEW
if [ -d "$THIRDPARTY_DIR/glew" ] && [ -f "$THIRDPARTY_DIR/glew/include/GL/glew.h" ]; then
    success "GLEW already present, skipping download"
else
    info "Downloading GLEW $GLEW_VERSION..."
    if curl -fsSL -o "$THIRDPARTY_DIR/glew.zip" \
        "https://github.com/nigels-com/glew/releases/download/glew-$GLEW_VERSION/glew-$GLEW_VERSION.zip"; then
        success "GLEW downloaded"
    else
        error "Failed to download GLEW. Check your internet connection."
        exit 1
    fi
    info "Extracting GLEW..."
    unzip -oq "$THIRDPARTY_DIR/glew.zip" -d "$THIRDPARTY_DIR"
    rm -f "$THIRDPARTY_DIR/glew.zip"
    # Rename to 'glew' (remove version suffix)
    rm -rf "$THIRDPARTY_DIR/glew"
    mv "$THIRDPARTY_DIR/glew-"* "$THIRDPARTY_DIR/glew"
    success "GLEW $GLEW_VERSION extracted"
fi

# ─── Step 4: Configure with CMake ────────────────────────────────────────────

step "Configuring project with CMake"

BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"

if cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" 2>&1; then
    success "CMake configuration complete"
else
    error "CMake configuration failed. See output above for details."
    exit 1
fi

# ─── Step 5: Build ───────────────────────────────────────────────────────────

step "Building waifuland (using $NPROC parallel jobs)"

if make -C "$BUILD_DIR" -j"$NPROC" 2>&1; then
    success "Build complete"
else
    error "Build failed. See output above for details."
    exit 1
fi

# ─── Done ─────────────────────────────────────────────────────────────────────

BINARY="$BUILD_DIR/bin/waifuland"

echo ""
echo -e "${GREEN}${BOLD}============================================${RESET}"
echo -e "${GREEN}${BOLD}  Build successful!${RESET}"
echo -e "${GREEN}${BOLD}============================================${RESET}"
echo ""
echo -e "  Binary:  ${BOLD}$BINARY${RESET}"
echo ""
echo -e "  Run it:"
echo -e "    ${BOLD}$BINARY${RESET}"
echo -e "    ${BOLD}$BINARY --models_dir /path/to/models${RESET}"
echo ""
echo -e "  Default model directory:"
echo -e "    ${BOLD}\$XDG_CONFIG_HOME/waifuland/models/${RESET}"
echo -e "    ${BOLD}~/.config/waifuland/models/${RESET}"
echo ""

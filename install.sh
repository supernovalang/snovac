#!/usr/bin/env bash
set -e

# ==============================================================================
# Snovalang Compiler (snovac) Universal One-Line Installer
# Usage: curl -fsSL https://raw.githubusercontent.com/supernovalang/snovac/master/install.sh | bash
# ==============================================================================

REPO="supernovalang/snovac"
INSTALL_DIR="${SNOVA_INSTALL_DIR:-$HOME/.snova/bin}"

# Text formatting
BOLD="$(tput bold 2>/dev/null || echo '')"
GREEN="$(tput setaf 2 2>/dev/null || echo '')"
CYAN="$(tput setaf 6 2>/dev/null || echo '')"
YELLOW="$(tput setaf 3 2>/dev/null || echo '')"
RED="$(tput setaf 1 2>/dev/null || echo '')"
RESET="$(tput sgr0 2>/dev/null || echo '')"

echo "${CYAN}${BOLD}"
cat << "BANNER"
  ____                                         
 / ___| _ __   _____   ____ _ _ __   ___ _ __  
 \___ \| '_ \ / _ \ \ / / _` | '_ \ / _ \ '__| 
  ___) | | | | (_) \ V / (_| | | | |  __/ |    
 |____/|_| |_|\___/ \_/ \__,_|_| |_|\___|_|    
           Snovalang Compiler (snovac)
BANNER
echo "${RESET}"

# 1. Detect Operating System
OS="$(uname -s)"
case "$OS" in
    Linux*)     PLATFORM="linux" ;;
    Darwin*)    PLATFORM="darwin" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT*)
        PLATFORM="windows"
        if command -v powershell.exe >/dev/null 2>&1; then
            echo "${CYAN}==> Windows environment detected. Invoking native Windows installer...${RESET}"
            SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" 2>/dev/null && pwd || echo ".")"
            if [ -f "$SCRIPT_DIR/scripts/install_windows.ps1" ]; then
                powershell.exe -ExecutionPolicy Bypass -File "$SCRIPT_DIR/scripts/install_windows.ps1"
                exit 0
            elif [ -f "$SCRIPT_DIR/install.ps1" ]; then
                powershell.exe -ExecutionPolicy Bypass -File "$SCRIPT_DIR/install.ps1"
                exit 0
            else
                powershell.exe -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/${REPO}/master/install.ps1 | iex"
                exit 0
            fi
        fi
        ;;
    *)
        echo "${RED}Error: Unsupported Operating System: $OS${RESET}" >&2
        exit 1
        ;;
esac

# 2. Detect Machine Architecture
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64)   ARCH_NAME="x86_64" ;;
    arm64|aarch64) ARCH_NAME="aarch64" ;;
    *)
        echo "${RED}Error: Unsupported CPU Architecture: $ARCH${RESET}" >&2
        exit 1
        ;;
esac

TARBALL="snovac-${PLATFORM}-${ARCH_NAME}.tar.gz"
DOWNLOAD_URL="https://github.com/${REPO}/releases/latest/download/${TARBALL}"

echo "${BOLD}==>${RESET} Detected target: ${GREEN}${PLATFORM}-${ARCH_NAME}${RESET}"
echo "${BOLD}==>${RESET} Downloading ${CYAN}${TARBALL}${RESET} from ${REPO}..."

TMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# 3. Download release binary
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$DOWNLOAD_URL" -o "$TMP_DIR/$TARBALL" || {
        echo "${YELLOW}Release tarball not found yet on latest release. Attempting source compile fallback...${RESET}"
        if command -v git >/dev/null 2>&1 && command -v make >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
            git clone --depth 1 "https://github.com/${REPO}.git" "$TMP_DIR/snovac-src"
            make -C "$TMP_DIR/snovac-src"
            mkdir -p "$TMP_DIR/extracted"
            cp "$TMP_DIR/snovac-src/build/snovac" "$TMP_DIR/extracted/snovac"
        else
            echo "${RED}Failed to download binary from $DOWNLOAD_URL${RESET}" >&2
            exit 1
        fi
    }
elif command -v wget >/dev/null 2>&1; then
    wget -qO "$TMP_DIR/$TARBALL" "$DOWNLOAD_URL" || {
        echo "${RED}Failed to download binary using wget${RESET}" >&2
        exit 1
    }
else
    echo "${RED}Error: curl or wget is required to install snovac.${RESET}" >&2
    exit 1
fi

if [ -f "$TMP_DIR/$TARBALL" ]; then
    mkdir -p "$TMP_DIR/extracted"
    tar -xzf "$TMP_DIR/$TARBALL" -C "$TMP_DIR/extracted"
fi

# 4. Install binary
mkdir -p "$INSTALL_DIR"
cp -f "$TMP_DIR/extracted/snovac" "$INSTALL_DIR/snovac"
chmod +x "$INSTALL_DIR/snovac"

echo "${GREEN}${BOLD}✓ Installed snovac binary into ${INSTALL_DIR}/snovac${RESET}"

# 5. Check and configure PATH
SHELL_CONFIG=""
case "$SHELL" in
    */zsh)  SHELL_CONFIG="$HOME/.zshrc" ;;
    */bash) SHELL_CONFIG="$HOME/.bashrc" ;;
    */fish) SHELL_CONFIG="$HOME/.config/fish/config.fish" ;;
    *)      SHELL_CONFIG="$HOME/.profile" ;;
esac

PATH_STR="export PATH=\"\$PATH:${INSTALL_DIR}\""

if [[ ":$PATH:" != *":${INSTALL_DIR}:"* ]]; then
    if [ -f "$SHELL_CONFIG" ]; then
        if ! grep -q "${INSTALL_DIR}" "$SHELL_CONFIG"; then
            echo "" >> "$SHELL_CONFIG"
            echo "# Snovalang Compiler" >> "$SHELL_CONFIG"
            echo "$PATH_STR" >> "$SHELL_CONFIG"
            echo "${YELLOW}Added ${INSTALL_DIR} to PATH in ${SHELL_CONFIG}${RESET}"
        fi
    fi
fi

echo ""
echo "${GREEN}${BOLD}Snovalang Compiler (snovac) was successfully installed!${RESET}"
echo "Run '${CYAN}snovac --help${RESET}' to get started."

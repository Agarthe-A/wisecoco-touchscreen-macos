#!/bin/sh
# Install or remove the wisecoco-touch-enable LaunchAgent for the current user.
# No sudo needed.
#
#   ./install.sh             build, install and start the agent
#   ./install.sh uninstall   stop and remove the agent and its binary
#
# SPDX-License-Identifier: MIT

set -eu

LABEL="io.github.wisecoco-touch-enable"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="$HOME/.local/bin"
BIN="$BIN_DIR/wisecoco-touch-enable"
AGENT_DIR="$HOME/Library/LaunchAgents"
PLIST_SRC="$HERE/launchd/$LABEL.plist"
PLIST_DST="$AGENT_DIR/$LABEL.plist"
LOG="$HOME/Library/Logs/wisecoco-touch-enable.log"
DOMAIN="gui/$(id -u)"

die() {
    echo "install.sh: $*" >&2
    exit 1
}

# Escape a string for use as a sed replacement with '|' as delimiter.
sed_escape() {
    printf '%s' "$1" | sed -e 's/[\\|&]/\\&/g'
}

stop_agent() {
    if launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
        launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
        sleep 1
    fi
}

do_install() {
    [ "$(uname -s)" = "Darwin" ] || die "macOS only"
    xcode-select -p >/dev/null 2>&1 \
        || die "the Command Line Tools are missing; run: xcode-select --install"

    make -C "$HERE"

    mkdir -p "$BIN_DIR" "$AGENT_DIR" "$(dirname "$LOG")"
    stop_agent
    # Replace rather than overwrite: a signed binary rewritten in place can be
    # killed by the kernel on the next launch.
    rm -f "$BIN"
    cp "$HERE/build/wisecoco-touch-enable" "$BIN"
    chmod 755 "$BIN"

    sed -e "s|__HOME__|$(sed_escape "$HOME")|g" \
        -e "s|__BIN__|$(sed_escape "$BIN")|g" \
        "$PLIST_SRC" > "$PLIST_DST"
    plutil -lint "$PLIST_DST" >/dev/null || die "generated plist is invalid: $PLIST_DST"

    launchctl bootstrap "$DOMAIN" "$PLIST_DST"

    echo "Installed $BIN"
    echo "LaunchAgent $PLIST_DST is running."
    echo "Log: $LOG"
}

do_uninstall() {
    stop_agent
    rm -f "$PLIST_DST" "$BIN"
    echo "Removed $LABEL (the log file $LOG was kept)."
}

case "${1:-install}" in
    install)   do_install ;;
    uninstall) do_uninstall ;;
    -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' ;;
    *)         die "usage: $0 [install|uninstall]" ;;
esac

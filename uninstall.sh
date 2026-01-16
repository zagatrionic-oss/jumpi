#!/bin/bash
# uninstall.sh - remove jumpi app and launcher

APPNAME="jumpi"
TARGETDIR="$HOME/.local/share/$APPNAME"
DESKTOPFILE="$APPNAME.desktop"

rm -rf "$TARGETDIR"
rm -f ~/.local/share/applications/$DESKTOPFILE

echo "Uninstalled $APPNAME. Files and launcher removed."

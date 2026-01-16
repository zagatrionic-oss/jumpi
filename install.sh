#!/bin/bash
# install.sh - copy all files into ~/.local/share/jumpi and create launcher

APPNAME="jumpi"
TARGETDIR="$HOME/.local/share/$APPNAME"
DESKTOPFILE="$APPNAME.desktop"

# Make target dir
mkdir -p "$TARGETDIR"

# Copy everything from current folder into target dir
cp -r ./* "$TARGETDIR/"

# Create .desktop entry
mkdir -p ~/.local/share/applications
cat > ~/.local/share/applications/$DESKTOPFILE <<EOF
[Desktop Entry]
Name=SUPER HARD Obby Game
Comment=VERY HARD
Exec=$TARGETDIR/jumpi
Icon=$TARGETDIR/logo.png
Type=Application
Categories=Game;
Terminal=false
StartupNotify=true
EOF

echo "Installed $APPNAME to $TARGETDIR. Launcher added to your application menu."

#!/bin/bash

# Compile the window manager
echo "Compiling window manager..."
gcc -Wall -O2 etyWM.c -o etyWM $(pkg-config --cflags --libs xcb-shape xcb cairo) -lm

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

# Create a simple xinitrc if it doesn't exist
if [ ! -f ~/.xinitrc ]; then
    echo "Creating ~/.xinitrc..."
    cat > ~/.xinitrc << 'EOL'
#!/bin/bash

# Load X resources
echo "Loading X resources..."
[[ -f ~/.Xresources ]] && xrdb -merge ~/.Xresources

# Ensure PATH is set correctly
export PATH="/usr/bin:/bin:$PATH"

# Start the window manager first
echo "Starting etyWM..."
$(pwd)/etyWM &

# Wait for WM to initialize
sleep 10

# Start xterm after WM is running
echo "Starting xterm..."
DISPLAY=:2 xterm 2>&1
EOL
    chmod +x ~/.xinitrc
fi

# Create a simple .desktop entry for display manager
echo "Creating display manager entry..."
sudo tee /usr/share/xsessions/etywm.desktop << EOL
[Desktop Entry]
Name=etyWM
Comment=Ety Window Manager
Exec=bash -c "$(pwd)/etyWM & sleep 2 && exec xterm -ls"
Type=Application
EOL

# Backup existing X server logs
if [ -f ~/.local/share/xorg/Xorg.0.log ]; then
    mv ~/.local/share/xorg/Xorg.0.log ~/.local/share/xorg/Xorg.0.log.old
fi

# Create log directory if it doesn't exist
mkdir -p ~/.local/share/xorg

echo "Setup complete! You can now:"
echo "1. Start X with 'startx' to run the window manager directly"
echo "2. Select 'etyWM' from your display manager login screen"
echo "3. Add 'exec $(pwd)/etyWM' to your existing .xinitrc if you have one"

# Optional: start X server immediately
read -p "Would you like to start the window manager now? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    startx
fi
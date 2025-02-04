#!/bin/bash

# Compile the window manager
echo "Compiling window manager..."
gcc -o etyWM etyWM.c -lX11 -lXext -lXrender -lm

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

# Kill any existing processes
echo "Cleaning up old processes..."
pkill Xephyr
pkill etyWM

# Start fresh X server
echo "Starting Xephyr..."
Xephyr :2 -ac -screen 1024x768 -reset -terminate 2>/dev/null &

# Wait for Xephyr to start
echo "Waiting for Xephyr to initialize..."
sleep 2

# Start the window manager
echo "Starting window manager..."
DISPLAY=:2 ./etyWM &

# Wait for WM to initialize
echo "Waiting for window manager to initialize..."
sleep 1

# Launch initial application
echo "Launching xclock..."
DISPLAY=:2 xclock &

sleep 1

# Launch initial application
echo "Launching xclock2..."
DISPLAY=:2 xclock &


echo "Setup complete! Your window manager is running."
echo "Use Alt + Left Mouse Button to move windows"
echo "Use Alt + F4 to close windows"
echo "Press Ctrl+C to exit"

# Wait for user interrupt
wait
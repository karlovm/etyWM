#!/bin/bash

# Compile the window manager
echo "Compiling window manager..."
gcc -Wall -O2 etyWM.c -o etyWM $(pkg-config --cflags --libs xcb-shape xcb cairo) -lm




if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

# Kill any existing processes
echo "Cleaning up old processes..."
pkill Xephyr
pkill etyWM

# Start fresh X server with Xephyr
echo "Starting Xephyr..."
# Enable error logging
set -x

# Start Xephyr with error output
Xephyr :2 -ac -screen 1279x720  -br -reset -terminate 2>&1 | tee xephyr.log &
XEPHYR_PID=$!

# Wait for Xephyr to start and verify it's running
sleep 2
if ! ps -p $XEPHYR_PID > /dev/null; then
    echo "Xephyr failed to start!"
    exit 1
fi

echo "Xephyr started with PID $XEPHYR_PID"

# Start the window manager with error output
DISPLAY=:2 ./etyWM 2>&1 | tee wm.log &
WM_PID=$!

# Wait and verify window manager is running
sleep 2
if ! ps -p $WM_PID > /dev/null; then
    echo "Window manager failed to start!"
    kill $XEPHYR_PID
    exit 1
fi

echo "Window manager started with PID $WM_PID"

# Try to start a terminal
sleep 1
DISPLAY=:2 xterm 2>&1 | tee xterm.log &

echo "Test environment is running"

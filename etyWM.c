#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_WINDOWS 64
#define BORDER_WIDTH 1  // Reduced border width for better appearance
#define BORDER_COLOR 0x666666    // Softer border color
#define HEADER_HEIGHT 20
#define CORNER_RADIUS 10

#define SCALE_FACTOR 4  // Increase resolution for smoother corners
#define PI 3.14159265358979323846


typedef struct {
    Window frame;
    Window client;
    Window header;
    int x, y;
    unsigned int width, height;
} WindowInfo;

Display *display;
Window root;
WindowInfo windows[MAX_WINDOWS];
int window_count = 0;
int dragging = 0;
int drag_start_x, drag_start_y;
Window current_frame = None;
Cursor cursor;
int screen_width, screen_height;

void set_rounded_corners(Window win, int width, int height) {
    // Create a mask pixmap
    Pixmap mask = XCreatePixmap(display, win, width, height, 1);
    if (!mask) {
        fprintf(stderr, "Failed to create mask pixmap\n");
        return;
    }

    // Create a GC for the mask
    XGCValues xgcv;
    GC shape_gc = XCreateGC(display, mask, 0, &xgcv);
    if (!shape_gc) {
        fprintf(stderr, "Failed to create GC\n");
        XFreePixmap(display, mask);
        return;
    }

    // Clear the mask
    XSetForeground(display, shape_gc, 0);
    XFillRectangle(display, mask, shape_gc, 0, 0, width, height);

    // Set the shape
    XSetForeground(display, shape_gc, 1);
    
    // Draw the main rectangle with rounded corners
    XFillRectangle(display, mask, shape_gc, 
                   CORNER_RADIUS, 0, 
                   width - 2 * CORNER_RADIUS, height);
    XFillRectangle(display, mask, shape_gc,
                   0, CORNER_RADIUS,
                   width, height - 2 * CORNER_RADIUS);

    // Draw the corners
    XFillArc(display, mask, shape_gc,
             0, 0,
             2 * CORNER_RADIUS, 2 * CORNER_RADIUS,
             0, 360 * 64);
    XFillArc(display, mask, shape_gc,
             width - 2 * CORNER_RADIUS, 0,
             2 * CORNER_RADIUS, 2 * CORNER_RADIUS,
             0, 360 * 64);
    XFillArc(display, mask, shape_gc,
             0, height - 2 * CORNER_RADIUS,
             2 * CORNER_RADIUS, 2 * CORNER_RADIUS,
             0, 360 * 64);
    XFillArc(display, mask, shape_gc,
             width - 2 * CORNER_RADIUS, height - 2 * CORNER_RADIUS,
             2 * CORNER_RADIUS, 2 * CORNER_RADIUS,
             0, 360 * 64);

    // Apply the shape mask
    XShapeCombineMask(display, win, ShapeBounding, 0, 0, mask, ShapeSet);

    // Clean up
    XFreeGC(display, shape_gc);
    XFreePixmap(display, mask);
}



void constrain_window_position(int *x, int *y, int width, int height) {
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x + width > screen_width) *x = screen_width - width;
    if (*y + height > screen_height) *y = screen_height - height;
}

void create_shaped_background(Window parent, int x, int y, int width, int height, unsigned long color) {
    Window win = XCreateSimpleWindow(
        display, parent,
        x, y, width, height,
        0, 0, color
    );
    
    set_rounded_corners(win, width, height);
    XMapWindow(display, win);
    return win;
}

void add_window(Window client) {
    if (window_count >= MAX_WINDOWS) return;

    XWindowAttributes attr;
    XGetWindowAttributes(display, client, &attr);

    // Calculate dimensions for the frame
    int frame_width = attr.width;
    int frame_height = attr.height + HEADER_HEIGHT;

    // Create background window (this will be our frame)
    Window frame = XCreateSimpleWindow(
        display, root,
        0, 0,
        frame_width, frame_height,
        BORDER_WIDTH, BORDER_COLOR, 0x333333
    );

    // Create a background window that will handle the shape
    Window bg = XCreateSimpleWindow(
        display, frame,
        0, 0,
        frame_width, frame_height,
        0, 0, 0x333333
    );

    // Create header with the same rounded top corners
    Window header = XCreateSimpleWindow(
        display, frame,
        0, 0,
        frame_width, HEADER_HEIGHT,
        0, 0, 0x666666
    );

    // Set event masks
    XSelectInput(display, frame, SubstructureRedirectMask | SubstructureNotifyMask);
    XSelectInput(display, header, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XSelectInput(display, client, StructureNotifyMask | EnterWindowMask);

    // Apply shape to the background window
    set_rounded_corners(frame, frame_width, frame_height);

    // Map windows in the correct order
    XMapWindow(display, frame);
    XMapWindow(display, bg);
    XMapWindow(display, header);
    
    // Reparent and map the client window
    XReparentWindow(display, client, frame, 0, HEADER_HEIGHT);
    XMoveResizeWindow(display, client, 0, HEADER_HEIGHT, 
                     attr.width, attr.height);
    XMapWindow(display, client);

    // Position the frame
    int x = (screen_width - frame_width) / 2;
    int y = (screen_height - frame_height) / 2;
    constrain_window_position(&x, &y, frame_width, frame_height);
    XMoveWindow(display, frame, x, y);

    // Store window information
    windows[window_count].frame = frame;
    windows[window_count].client = client;
    windows[window_count].header = header;
    windows[window_count].x = x;
    windows[window_count].y = y;
    windows[window_count].width = frame_width;
    windows[window_count].height = frame_height;
    window_count++;

    // Force update
    XSync(display, False);
}

void remove_window(Window w) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].client == w || windows[i].frame == w) {
            XDestroyWindow(display, windows[i].frame);
            for (int j = i; j < window_count - 1; j++) {
                windows[j] = windows[j + 1];
            }
            window_count--;
            break;
        }
    }
}

void handle_map_request(XEvent *e) {
    XMapRequestEvent *event = &e->xmaprequest;
    
    // Check if window is already managed
    for (int i = 0; i < window_count; i++) {
        if (windows[i].client == event->window) {
            XMapWindow(display, event->window);
            return;
        }
    }
    
    add_window(event->window);
}

void handle_button_press(XEvent *e) {
    XButtonEvent *event = &e->xbutton;
    
    // Check if header was clicked
    for (int i = 0; i < window_count; i++) {
        if (windows[i].header == event->window) {
            XWindowAttributes attr;
            XGetWindowAttributes(display, windows[i].frame, &attr);
            drag_start_x = event->x_root - attr.x;
            drag_start_y = event->y_root - attr.y;
            current_frame = windows[i].frame;
            dragging = 1;
            XRaiseWindow(display, current_frame);
            return;
        }
    }

    // Handle Alt+click
    if ((event->state & Mod1Mask) && event->subwindow != None) {
        for (int i = 0; i < window_count; i++) {
            if (windows[i].client == event->subwindow) {
                XWindowAttributes attr;
                XGetWindowAttributes(display, windows[i].frame, &attr);
                drag_start_x = event->x_root - attr.x;
                drag_start_y = event->y_root - attr.y;
                current_frame = windows[i].frame;
                dragging = 1;
                XRaiseWindow(display, current_frame);
                return;
            }
        }
    }
}

void handle_motion_notify(XEvent *e) {
    if (!dragging) return;
    
    XMotionEvent *event = &e->xmotion;
    int new_x = event->x_root - drag_start_x;
    int new_y = event->y_root - drag_start_y;
    
    // Get window attributes for constraint calculation
    XWindowAttributes attr;
    XGetWindowAttributes(display, current_frame, &attr);
    
    constrain_window_position(&new_x, &new_y, attr.width, attr.height);
    XMoveWindow(display, current_frame, new_x, new_y);
}

int main(void) {
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }
    
    root = DefaultRootWindow(display);
    Screen *screen = DefaultScreenOfDisplay(display);
    screen_width = WidthOfScreen(screen);
    screen_height = HeightOfScreen(screen);
    
    cursor = XCreateFontCursor(display, XC_left_ptr);
    XDefineCursor(display, root, cursor);
    
    XSetWindowBackground(display, root, 0x333333);
    XClearWindow(display, root);
    
    XSelectInput(display, root,
                SubstructureRedirectMask | SubstructureNotifyMask |
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    
    // Grab Alt+click for window dragging
    XGrabButton(display, Button1, Mod1Mask, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, cursor);
    
    // Grab Alt+F4 for closing windows
    XGrabKey(display, XKeysymToKeycode(display, XK_F4), Mod1Mask,
             root, True, GrabModeAsync, GrabModeAsync);

    XEvent ev;
    while (1) {
        XNextEvent(display, &ev);
        switch (ev.type) {
            case MapRequest:
                handle_map_request(&ev);
                break;
            case DestroyNotify:
                remove_window(ev.xdestroywindow.window);
                break;
            case ButtonPress:
                handle_button_press(&ev);
                break;
            case ButtonRelease:
                dragging = 0;
                current_frame = None;
                break;
            case MotionNotify:
                handle_motion_notify(&ev);
                break;
            case KeyPress:
                if (ev.xkey.keycode == XKeysymToKeycode(display, XK_F4) &&
                    ev.xkey.state & Mod1Mask) {
                    for (int i = 0; i < window_count; i++) {
                        if (windows[i].frame == ev.xkey.subwindow) {
                            XDestroyWindow(display, windows[i].frame);
                            break;
                        }
                    }
                }
                break;
        }
    }
    
    XFreeCursor(display, cursor);
    XCloseDisplay(display);
    return 0;
}
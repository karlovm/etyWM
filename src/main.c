/**
 * @file etywm.c
 * @brief A simple X11 window manager using XCB.
 *
 * This file implements window management features such as dragging,
 * resizing, fullscreen toggling, and launching external helper programs.
 * It also demonstrates how to set a background pixmap for the root window.
 *
 * Author: Mikhail Karlov
 * Date: 2025-02-05
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include "config.h"
#include "client.h"
#include "draw.h"

/* Global variables for dragging/resizing state */
static int dragging = 0;
static int drag_start_x = 0, drag_start_y = 0;
static int frame_start_x = 0, frame_start_y = 0;
static Client *drag_client = NULL;

static int resizing = 0;
static int resize_start_x = 0, resize_start_y = 0;
static int orig_frame_x = 0;
static int orig_frame_y = 0;
static int orig_frame_width = 0;
static int orig_frame_height = 0;
static int resize_flags = 0;
static Client *resize_client = NULL;

/* For double-click detection on the title bar */
static xcb_timestamp_t last_click_time = 0;

/**
 * @brief Initiates the window dragging process.
 *
 * This function records the starting positions for both the pointer and
 * the window frame, grabs the pointer for receiving motion events, and logs
 * an error if the pointer grab fails.
 *
 * @param conn Pointer to the XCB connection.
 * @param c Pointer to the Client structure representing the window.
 * @param pointer_x The X coordinate of the pointer at drag start.
 * @param pointer_y The Y coordinate of the pointer at drag start.
 */
void start_drag(xcb_connection_t *conn, Client *c, int pointer_x, int pointer_y)
{
    dragging = 1;
    drag_client = c;
    drag_start_x = pointer_x;
    drag_start_y = pointer_y;

    /* Get current geometry of the frame */
    xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
    if (geo) {
        frame_start_x = geo->x;
        frame_start_y = geo->y;
        free(geo);
    } else {
        fprintf(stderr, "Error: Failed to get geometry for frame 0x%x\n", c->frame);
    }

    /* Grab pointer to capture motion events for dragging */
    xcb_grab_pointer_cookie_t gp_cookie = xcb_grab_pointer(conn, 1, c->frame,
                                                           XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                                           XCB_WINDOW_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *gp_reply = xcb_grab_pointer_reply(conn, gp_cookie, NULL);
    if (!gp_reply || gp_reply->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to grab pointer for moving window (frame 0x%x)\n", c->frame);
        dragging = 0;
    } else {
        fprintf(stderr, "etyWM Log: Pointer grabbed for dragging window (frame 0x%x)\n", c->frame);
    }
    free(gp_reply);
}

/**
 * @brief Ends the dragging process.
 *
 * Ungrabs the pointer and resets dragging-related variables.
 *
 * @param conn Pointer to the XCB connection.
 */
void end_drag(xcb_connection_t *conn)
{
    dragging = 0;
    drag_client = NULL;
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    fprintf(stderr, "etyWM Log: Dragging ended and pointer ungrabbed\n");
}

/**
 * @brief Initiates the window resizing process.
 *
 * Records the starting positions and geometry for the window and its frame,
 * grabs the pointer for motion events, and logs an error if the pointer grab fails.
 *
 * @param conn Pointer to the XCB connection.
 * @param c Pointer to the Client structure representing the window.
 * @param pointer_x The X coordinate of the pointer at resize start.
 * @param pointer_y The Y coordinate of the pointer at resize start.
 * @param flags Flags indicating which edges are being resized.
 */
void start_resize(xcb_connection_t *conn, Client *c, int pointer_x, int pointer_y, int flags)
{
    if (c->state == STATE_FULLSCREEN) {
        fprintf(stderr, "Warning: Attempted to resize fullscreen window (frame 0x%x); ignoring request\n", c->frame);
        return;
    }

    resizing = 1;
    resize_client = c;
    resize_start_x = pointer_x;
    resize_start_y = pointer_y;
    resize_flags = flags;

    /* Get current geometry of the frame */
    xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
    if (geo) {
        orig_frame_x = geo->x;
        orig_frame_y = geo->y;
        orig_frame_width = geo->width;
        orig_frame_height = geo->height;
        free(geo);
    } else {
        fprintf(stderr, "Error: Failed to get geometry for resizing frame 0x%x\n", c->frame);
    }

    /* Grab pointer to capture motion events for resizing */
    xcb_grab_pointer_cookie_t gp_cookie = xcb_grab_pointer(conn, 1, c->frame,
                                                           XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                                           XCB_WINDOW_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *gp_reply = xcb_grab_pointer_reply(conn, gp_cookie, NULL);
    if (!gp_reply || gp_reply->status != XCB_GRAB_STATUS_SUCCESS) {
        fprintf(stderr, "Error: Failed to grab pointer for resizing window (frame 0x%x)\n", c->frame);
        resizing = 0;
    } else {
        fprintf(stderr, "etyWM Log: Pointer grabbed for resizing window (frame 0x%x)\n", c->frame);
    }
    free(gp_reply);
}

/**
 * @brief Ends the resizing process.
 *
 * Ungrabs the pointer and resets resizing-related variables.
 *
 * @param conn Pointer to the XCB connection.
 */
void end_resize(xcb_connection_t *conn)
{
    resizing = 0;
    resize_client = NULL;
    resize_flags = 0;
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    fprintf(stderr, "etyWM Log: Resizing ended and pointer ungrabbed\n");
}

/**
 * @brief Updates the window size and position during a resize.
 *
 * Calculates the new geometry based on the pointer movement and applies the changes
 * to the window's frame, title, and client subwindows. Enforces minimum width and height.
 *
 * @param conn Pointer to the XCB connection.
 * @param pointer_x The current X coordinate of the pointer.
 * @param pointer_y The current Y coordinate of the pointer.
 */
void update_resize(xcb_connection_t *conn, int pointer_x, int pointer_y)
{
    int dx = pointer_x - resize_start_x;
    int dy = pointer_y - resize_start_y;
    int new_x = orig_frame_x;
    int new_y = orig_frame_y;
    int new_width = orig_frame_width;
    int new_height = orig_frame_height;

    /* Adjust geometry based on the resizing flags */
    if (resize_flags & RESIZE_LEFT) {
        new_x = orig_frame_x + dx;
        new_width = orig_frame_width - dx;
    }
    if (resize_flags & RESIZE_RIGHT)
        new_width = orig_frame_width + dx;
    if (resize_flags & RESIZE_TOP) {
        new_y = orig_frame_y + dy;
        new_height = orig_frame_height - dy;
    }
    if (resize_flags & RESIZE_BOTTOM)
        new_height = orig_frame_height + dy;

    /* Enforce minimum dimensions */
    if (new_width < MIN_WIDTH) {
        if (resize_flags & RESIZE_LEFT)
            new_x = orig_frame_x + (orig_frame_width - MIN_WIDTH);
        new_width = MIN_WIDTH;
    }
    if (new_height < MIN_HEIGHT) {
        if (resize_flags & RESIZE_TOP)
            new_y = orig_frame_y + (orig_frame_height - MIN_HEIGHT);
        new_height = MIN_HEIGHT;
    }

    /* Configure the frame window with the new geometry */
    uint32_t values[4] = { new_x, new_y, new_width, new_height };
    xcb_configure_window(conn, resize_client->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    /* Adjust the title bar window */
    values[0] = RESIZE_BORDER;
    values[1] = 0;
    values[2] = new_width - 2 * RESIZE_BORDER;
    values[3] = TITLE_BAR_HEIGHT;
    xcb_configure_window(conn, resize_client->title,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    /* Adjust the client (content) window */
    values[0] = RESIZE_BORDER;
    values[1] = TITLE_BAR_HEIGHT;
    values[2] = new_width - 2 * RESIZE_BORDER;
    values[3] = new_height - TITLE_BAR_HEIGHT - RESIZE_BORDER;
    xcb_configure_window(conn, resize_client->client,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    /* Set rounded corners if desired */
    set_rounded_corners(conn, resize_client->frame, new_width, new_height, CORNER_RADIUS);
    xcb_flush(conn);
    fprintf(stderr, "etyWM Log: Window (frame 0x%x) resized to %dx%d at (%d,%d)\n",
            resize_client->frame, new_width, new_height, new_x, new_y);
}

/**
 * @brief Launches the picom compositor.
 *
 * Forks the process and starts the compositor with the specified configuration file.
 */
void launch_picom(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process: Create a new session and execute picom */
        setsid();
        execlp("picom", "picom", "--config", "/home/serio/.config/picom.conf", NULL);
        fprintf(stderr, "Error: Failed to launch picom\n");
        exit(1);
    } else if (pid < 0) {
        fprintf(stderr, "Error: fork() failed when trying to launch picom\n");
    } else {
        fprintf(stderr, "etyWM Log: Launched picom (pid %d)\n", pid);
    }
}

/**
 * @brief Launches the xterm terminal emulator.
 *
 * Forks the process and starts xterm.
 */
void launch_xterm(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process: Create a new session and execute xterm */
        setsid();
        execl("/usr/bin/xterm", "xterm", NULL);
        fprintf(stderr, "Error: Failed to launch xterm\n");
        exit(1);
    } else if (pid < 0) {
        fprintf(stderr, "Error: fork() failed when trying to launch xterm\n");
    } else {
        fprintf(stderr, "etyWM Log: Launched xterm (pid %d)\n", pid);
    }
}

/**
 * @brief Main function for the window manager.
 *
 * Connects to the X server, sets up the root window events and background, launches helper
 * programs, and enters the main event loop.
 *
 * @return EXIT_SUCCESS on normal termination, or EXIT_FAILURE on error.
 */
int main(void)
{
    /* Connect to the X server using XCB */
    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "Error: Cannot open display\n");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "etyWM Log: Connected to X server\n");

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    xcb_screen_t *screen = iter.data;

    /* Request events on the root window */
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(conn, screen->root,
                                                                    XCB_CW_EVENT_MASK, &mask);
    if (xcb_request_check(conn, cookie)) {
        fprintf(stderr, "Error: Another window manager is already running.\n");
        xcb_disconnect(conn);
        exit(EXIT_FAILURE);
    }
    xcb_flush(conn);
    fprintf(stderr, "etyWM Log: Substructure events selected on root window\n");

    /* Launch external helper programs */
   // launch_picom();
    launch_xterm();

    /* Create and set a background pixmap */
    xcb_pixmap_t bg_pixmap = create_background_pixmap(conn, screen, "/home/serio/etyWM/background_sm.png");
    if (bg_pixmap != XCB_NONE) {
        uint32_t value = bg_pixmap;
        xcb_change_window_attributes(conn, screen->root, XCB_CW_BACK_PIXMAP, &value);
        xcb_clear_area(conn, 0, screen->root, 0, 0,
                       screen->width_in_pixels, screen->height_in_pixels);

        /* Set _XROOTPMAP_ID for compositors */
        xcb_intern_atom_cookie_t cookie1 = xcb_intern_atom(conn, 0, strlen("_XROOTPMAP_ID"), "_XROOTPMAP_ID");
        xcb_intern_atom_reply_t *reply1 = xcb_intern_atom_reply(conn, cookie1, NULL);
        if (reply1) {
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                                reply1->atom, XCB_ATOM_PIXMAP, 32, 1,
                                (unsigned char *)&bg_pixmap);
            free(reply1);
        } else {
            fprintf(stderr, "Warning: Failed to set _XROOTPMAP_ID property\n");
        }

        /* Set ESETROOT_PMAP_ID for some compositors */
        xcb_intern_atom_cookie_t cookie2 = xcb_intern_atom(conn, 0, strlen("ESETROOT_PMAP_ID"), "ESETROOT_PMAP_ID");
        xcb_intern_atom_reply_t *reply2 = xcb_intern_atom_reply(conn, cookie2, NULL);
        if (reply2) {
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                                reply2->atom, XCB_ATOM_PIXMAP, 32, 1,
                                (unsigned char *)&bg_pixmap);
            free(reply2);
        } else {
            fprintf(stderr, "Warning: Failed to set ESETROOT_PMAP_ID property\n");
        }
        fprintf(stderr, "etyWM Log: Background pixmap set successfully\n");
    } else {
        fprintf(stderr, "Error: Failed to create background pixmap\n");
    }
    xcb_flush(conn);

    /* Main event loop */
    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn))) {
        uint8_t response = event->response_type & ~0x80;
        switch (response) {
            case XCB_MAP_REQUEST: {
                xcb_map_request_event_t *map_req = (xcb_map_request_event_t *)event;
                fprintf(stderr, "etyWM Log: MAP_REQUEST for window 0x%x\n", map_req->window);
                create_frame(conn, screen, map_req->window);
                break;
            }
            case XCB_UNMAP_NOTIFY: {
                xcb_unmap_notify_event_t *unmap = (xcb_unmap_notify_event_t *)event;
                Client *c = find_client(unmap->window);
                if (c && unmap->window == c->client) {
                    fprintf(stderr, "etyWM Log: UNMAP_NOTIFY for client window 0x%x; unmapping frame 0x%x\n", c->client, c->frame);
                    xcb_unmap_window(conn, c->frame);
                    xcb_flush(conn);
                }
                break;
            }
            case XCB_CONFIGURE_REQUEST: {
                xcb_configure_request_event_t *cfg_req = (xcb_configure_request_event_t *)event;
                Client *c = find_client(cfg_req->window);
                if (c && cfg_req->window == c->client) {
                    uint32_t values[7];
                    int i = 0;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
                        values[i++] = cfg_req->width;
                        uint32_t frame_width = cfg_req->width + 2 * RESIZE_BORDER;
                        xcb_configure_window(conn, c->frame, XCB_CONFIG_WINDOW_WIDTH, &frame_width);
                    }
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
                        values[i++] = cfg_req->height;
                        uint32_t frame_height = cfg_req->height + TITLE_BAR_HEIGHT + RESIZE_BORDER;
                        xcb_configure_window(conn, c->frame, XCB_CONFIG_WINDOW_HEIGHT, &frame_height);
                    }
                    if (i)
                        xcb_configure_window(conn, cfg_req->window, cfg_req->value_mask, values);
                } else {
                    /* For non-managed windows, simply forward the configure request */
                    uint32_t values[7];
                    int i = 0;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_X)
                        values[i++] = cfg_req->x;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_Y)
                        values[i++] = cfg_req->y;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_WIDTH)
                        values[i++] = cfg_req->width;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
                        values[i++] = cfg_req->height;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
                        values[i++] = cfg_req->border_width;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_SIBLING)
                        values[i++] = cfg_req->sibling;
                    if (cfg_req->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
                        values[i++] = cfg_req->stack_mode;
                    xcb_configure_window(conn, cfg_req->window, cfg_req->value_mask, values);
                }
                xcb_flush(conn);
                break;
            }
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *bp = (xcb_button_press_event_t *)event;
                Client *c = find_client(bp->event);
                if (!c)
                    c = find_client(bp->child);
                if (c) {
                    /* Raise the window */
                    uint32_t values[] = { XCB_STACK_MODE_ABOVE };
                    xcb_configure_window(conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE, values);

                    /* Right-click closes the window */
                    if (bp->detail == 3) {
                        fprintf(stderr, "etyWM Log: Right-click detected; destroying client (frame 0x%x)\n", c->frame);
                        destroy_client(conn, c);
                    } else if (bp->detail == 1) {
                        if (bp->event == c->title) {
                            /* Check for double-click on the title bar for toggling fullscreen */
                            if (bp->time - last_click_time < 300) {
                                fprintf(stderr, "etyWM Log: Double-click detected on title bar; toggling fullscreen (frame 0x%x)\n", c->frame);
                                toggle_fullscreen(conn, screen, c);
                                last_click_time = 0;
                            } else {
                                last_click_time = bp->time;
                                fprintf(stderr, "etyWM Log: Single-click detected on title bar; starting drag (frame 0x%x)\n", c->frame);
                                start_drag(conn, c, bp->root_x, bp->root_y);
                            }
                        } else {
                            /* Determine if a resize should be started based on pointer location */
                            int rel_x = bp->event_x;
                            int rel_y = bp->event_y;
                            xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
                            xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
                            if (!geo) {
                                fprintf(stderr, "Error: Failed to get geometry during button press (frame 0x%x)\n", c->frame);
                                break;
                            }
                            int fw = geo->width, fh = geo->height;
                            free(geo);
                            int flags = 0;
                            if (rel_x < RESIZE_BORDER)
                                flags |= RESIZE_LEFT;
                            if (rel_x > (fw - RESIZE_BORDER))
                                flags |= RESIZE_RIGHT;
                            if (rel_y < RESIZE_BORDER)
                                flags |= RESIZE_TOP;
                            if (rel_y > (fh - RESIZE_BORDER))
                                flags |= RESIZE_BOTTOM;
                            if (flags) {
                                fprintf(stderr, "etyWM Log: Starting resize (frame 0x%x) with flags 0x%x\n", c->frame, flags);
                                start_resize(conn, c, bp->root_x, bp->root_y, flags);
                            }
                        }
                    }
                    xcb_flush(conn);
                }
                break;
            }
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)event;
                if (dragging && drag_client) {
                    int dx = motion->root_x - drag_start_x;
                    int dy = motion->root_y - drag_start_y;
                    int new_x = frame_start_x + dx;
                    int new_y = frame_start_y + dy;
                    uint32_t values[2] = { new_x, new_y };
                    xcb_configure_window(conn, drag_client->frame,
                                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
                    xcb_flush(conn);
                } else if (resizing && resize_client) {
                    update_resize(conn, motion->root_x, motion->root_y);
                }
                break;
            }
            case XCB_BUTTON_RELEASE: {
                if (dragging) {
                    end_drag(conn);
                }
                if (resizing) {
                    end_resize(conn);
                }
                break;
            }
            case XCB_DESTROY_NOTIFY: {
                xcb_destroy_notify_event_t *dn = (xcb_destroy_notify_event_t *)event;
                Client *c = find_client(dn->window);
                if (c && dn->window == c->client) {
                    fprintf(stderr, "etyWM Log: DESTROY_NOTIFY for client window 0x%x; destroying frame 0x%x\n", c->client, c->frame);
                    xcb_destroy_window(conn, c->frame);
                    remove_client_by_frame(c->frame);
                }
                break;
            }
            default:
                break;
        }
        free(event);
    }

    fprintf(stderr, "etyWM Log: Exiting window manager\n");
    xcb_disconnect(conn);
    return 0;
}

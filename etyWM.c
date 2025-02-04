/*
 * etyWM - A minimal, fully functional XCB-based window manager with
 *         rounded window corners, focus-raising, and full mouse resizing support.
 *
 *
 * IMPORTANT: For translucency to work, a compositing manager must be running.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shape.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <unistd.h>
#include <sys/types.h>

#define TITLE_BAR_HEIGHT 24
#define BORDER_WIDTH 0
#define BUTTON_MARGIN 0
#define CORNER_RADIUS 16
#define RESIZE_BORDER 0
#define MIN_WIDTH 100
#define MIN_HEIGHT 50

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Resize edge flags */
#define RESIZE_LEFT (1 << 0)
#define RESIZE_RIGHT (1 << 1)
#define RESIZE_TOP (1 << 2)
#define RESIZE_BOTTOM (1 << 3)

/* Window state flags */
#define STATE_NORMAL 0
#define STATE_FULLSCREEN 1

/* Global XCB connection and screen */
xcb_connection_t *conn;
xcb_screen_t *screen;

typedef struct Client
{
    xcb_window_t client;
    xcb_window_t frame;
    xcb_window_t title;
    int state;            /* Window state (normal/fullscreen) */
    int saved_x, saved_y; /* Saved position for fullscreen restore */
    int saved_w, saved_h; /* Saved size for fullscreen restore */
    struct Client *next;
} Client;

Client *clients = NULL;

/* Dragging globals */
int dragging = 0;
int drag_start_x = 0;
int drag_start_y = 0;
int frame_start_x = 0;
int frame_start_y = 0;
Client *drag_client = NULL;

/* Resizing globals */
int resizing = 0;
int resize_start_x = 0, resize_start_y = 0;
int orig_frame_x = 0, orig_frame_y = 0;
int orig_frame_width = 0, orig_frame_height = 0;
int resize_flags = 0;
Client *resize_client = NULL;

/* For double-click detection on the title bar */
static xcb_timestamp_t last_click_time = 0;

/* Helper: add a new client to our list */
void add_client(Client *c)
{
    c->next = clients;
    clients = c;
}

/* Helper: remove a client from our list given its frame window */
void remove_client_by_frame(xcb_window_t frame)
{
    Client **curr = &clients;
    while (*curr)
    {
        if ((*curr)->frame == frame)
        {
            Client *tmp = *curr;
            *curr = (*curr)->next;
            free(tmp);
            return;
        }
        curr = &((*curr)->next);
    }
}

void send_configure_notify(xcb_window_t window, int x, int y, int width, int height)
{
    xcb_configure_notify_event_t event = {
        .response_type = XCB_CONFIGURE_NOTIFY,
        .event = window,
        .window = window,
        .above_sibling = XCB_NONE,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .border_width = 0,
        .override_redirect = 0};
    xcb_send_event(conn, 0, window,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                   (char *)&event);
}

/* Helper: find a client by a given window (frame, title bar, or client) */
Client *find_client(xcb_window_t win)
{
    Client *c = clients;
    while (c)
    {
        if (c->frame == win || c->title == win || c->client == win)
            return c;
        c = c->next;
    }
    return NULL;
}

/* Helper: check if a window still exists */
int window_exists(xcb_window_t win)
{
    xcb_get_window_attributes_cookie_t cookie = xcb_get_window_attributes(conn, win);
    xcb_get_window_attributes_reply_t *reply = xcb_get_window_attributes_reply(conn, cookie, NULL);
    if (reply)
    {
        free(reply);
        return 1;
    }
    return 0;
}

/* Helper function: create a rounded rectangle mask for a window.
 * Uses Cairo to draw a rounded rectangle into an A1 surface and then transfers
 * that data into a 1-bit pixmap used as the shape mask.
 */
void set_rounded_corners(xcb_window_t frame, int width, int height, int radius)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A1, width, height);
    cairo_t *cr = cairo_create(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgb(cr, 1, 1, 1);
    double r = radius;
    cairo_move_to(cr, r, 0);
    cairo_line_to(cr, width - r, 0);
    cairo_arc(cr, width - r, r, r, -M_PI / 2, 0);
    cairo_line_to(cr, width, height - r);
    cairo_arc(cr, width - r, height - r, r, 0, M_PI / 2);
    cairo_line_to(cr, r, height);
    cairo_arc(cr, r, height - r, r, M_PI / 2, M_PI);
    cairo_line_to(cr, 0, r);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_surface_flush(surface);

    xcb_pixmap_t mask_pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 1, mask_pixmap, frame, width, height);

    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, mask_pixmap, 0, NULL);

    int stride = cairo_image_surface_get_stride(surface);
    int data_len = stride * height;
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, mask_pixmap, gc,
                  width, height, 0, 0, 0, 1, data_len,
                  (const char *)cairo_image_surface_get_data(surface));

    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, frame, 0, 0, mask_pixmap);

    xcb_free_gc(conn, gc);
    xcb_free_pixmap(conn, mask_pixmap);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void toggle_fullscreen(Client *c)
{
    if (c->state == STATE_NORMAL)
    {
        /* Save current window geometry */
        xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
        xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
        if (geo)
        {
            c->saved_x = geo->x;
            c->saved_y = geo->y;
            c->saved_w = geo->width;
            c->saved_h = geo->height;
            free(geo);
        }

        /* Set fullscreen geometry */
        uint32_t values[4] = {
            0, 0,                    /* x, y */
            screen->width_in_pixels, /* width */
            screen->height_in_pixels /* height */
        };
        xcb_configure_window(conn, c->frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        /* Update client window size */
        values[0] = RESIZE_BORDER;
        values[1] = TITLE_BAR_HEIGHT;
        values[2] = screen->width_in_pixels - 2 * RESIZE_BORDER;
        values[3] = screen->height_in_pixels - TITLE_BAR_HEIGHT - RESIZE_BORDER;
        xcb_configure_window(conn, c->client,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        /* Update title bar */
        values[0] = RESIZE_BORDER;
        values[1] = 0;
        values[2] = screen->width_in_pixels - 2 * RESIZE_BORDER;
        values[3] = TITLE_BAR_HEIGHT;
        xcb_configure_window(conn, c->title,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        c->state = STATE_FULLSCREEN;
    }
    else
    {
        /* Restore saved geometry */
        uint32_t values[4] = {
            c->saved_x, c->saved_y,
            c->saved_w, c->saved_h};
        xcb_configure_window(conn, c->frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        /* Update client window */
        values[0] = RESIZE_BORDER;
        values[1] = TITLE_BAR_HEIGHT;
        values[2] = c->saved_w - 2 * RESIZE_BORDER;
        values[3] = c->saved_h - TITLE_BAR_HEIGHT - RESIZE_BORDER;
        xcb_configure_window(conn, c->client,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        /* Update title bar */
        values[0] = RESIZE_BORDER;
        values[1] = 0;
        values[2] = c->saved_w - 2 * RESIZE_BORDER;
        values[3] = TITLE_BAR_HEIGHT;
        xcb_configure_window(conn, c->title,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        c->state = STATE_NORMAL;
    }

    /* Update rounded corners */
    set_rounded_corners(c->frame,
                        c->state == STATE_NORMAL ? c->saved_w : screen->width_in_pixels,
                        c->state == STATE_NORMAL ? c->saved_h : screen->height_in_pixels,
                        CORNER_RADIUS);

    xcb_flush(conn);
}

/*
 * create_frame: Creates a frame for a client window. In this version the frame
 * is enlarged by RESIZE_BORDER on the left, right, and bottom to allow resizing.
 *
 * The frame layout is as follows:
 *  - The frame window’s size = client_width + 2*RESIZE_BORDER by client_height + TITLE_BAR_HEIGHT + RESIZE_BORDER.
 *  - The title bar is positioned at (RESIZE_BORDER, 0) and spans the width minus the side borders.
 *  - The client window is reparented at (RESIZE_BORDER, TITLE_BAR_HEIGHT).
 *
 * Note: Only the title bar is made translucent (90% opacity) via _NET_WM_WINDOW_OPACITY.
 */
void create_frame(xcb_window_t client)
{
    xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, client);
    xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn, geom_cookie, NULL);
    if (!geom)
        return;

    int frame_x = geom->x;
    int frame_y = geom->y;
    int client_width = geom->width;
    int client_height = geom->height;
    int frame_width = client_width + 2 * RESIZE_BORDER;
    int frame_height = client_height + TITLE_BAR_HEIGHT + RESIZE_BORDER;

    /* Create frame with a normal (opaque) background */
    xcb_window_t frame = xcb_generate_id(conn);
    uint32_t frame_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t frame_values[2];
    frame_values[0] = screen->white_pixel;
    frame_values[1] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS |
                      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_POINTER_MOTION;
    xcb_create_window(conn,
                      XCB_COPY_FROM_PARENT,
                      frame,
                      screen->root,
                      frame_x, frame_y,
                      frame_width, frame_height,
                      0, /* No visible border */
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual,
                      frame_mask, frame_values);

    set_rounded_corners(frame, frame_width, frame_height, CORNER_RADIUS);

    /* Create title bar */
    xcb_window_t title = xcb_generate_id(conn);
    uint32_t title_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t title_values[2];
    title_values[0] = 0xD0D0D0; /* light gray */
    title_values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS;
    xcb_create_window(conn,
                      XCB_COPY_FROM_PARENT,
                      title,
                      frame,
                      RESIZE_BORDER, 0,
                      frame_width - 2 * RESIZE_BORDER, TITLE_BAR_HEIGHT,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual,
                      title_mask, title_values);

    /* Set WM_NAME property for the title bar (if needed for picom matching) */
    const char *title_name = "etyWM_title";
    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        title,
                        XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING,
                        8,
                        strlen(title_name),
                        title_name);

    /* Set the _NET_WM_WINDOW_OPACITY property for translucency */
    xcb_intern_atom_cookie_t opacity_cookie = xcb_intern_atom(conn, 0,
                                                              strlen("_NET_WM_WINDOW_OPACITY"),
                                                              "_NET_WM_WINDOW_OPACITY");
    xcb_intern_atom_reply_t *opacity_reply = xcb_intern_atom_reply(conn, opacity_cookie, NULL);
    uint32_t opacity = 0xE6E6E6E6;
    xcb_change_property(conn,
                        XCB_PROP_MODE_REPLACE,
                        title,
                        opacity_reply->atom,
                        XCB_ATOM_CARDINAL,
                        32,
                        1,
                        &opacity);
    free(opacity_reply);
    /* Reparent client window */
    xcb_reparent_window(conn, client, frame, RESIZE_BORDER, TITLE_BAR_HEIGHT);
    uint32_t border_width = 0;
    xcb_configure_window(conn, client, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);

    /* Map all windows */
    xcb_map_window(conn, client);
    xcb_map_window(conn, title);
    xcb_map_window(conn, frame);
    xcb_flush(conn);
    free(geom);

    /* Add client to our list */
    Client *c = malloc(sizeof(Client));
    if (!c)
    {
        fprintf(stderr, "Out of memory\n");
        exit(EXIT_FAILURE);
    }
    c->client = client;
    c->frame = frame;
    c->title = title;
    c->state = STATE_NORMAL;
    c->next = NULL;
    add_client(c);
}

/* Destroy a client (frame, title, and the client window) */
void destroy_client(Client *c)
{
    xcb_kill_client(conn, c->client);
    xcb_destroy_window(conn, c->frame);
    xcb_flush(conn);
    remove_client_by_frame(c->frame);
}

/* Start dragging a window by grabbing the pointer */
void start_drag(Client *c, int pointer_x, int pointer_y)
{
    dragging = 1;
    drag_client = c;
    drag_start_x = pointer_x;
    drag_start_y = pointer_y;
    xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
    if (geo)
    {
        frame_start_x = geo->x;
        frame_start_y = geo->y;
        free(geo);
    }
    xcb_grab_pointer_cookie_t gp_cookie = xcb_grab_pointer(conn, 1, screen->root,
                                                           XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                                           XCB_WINDOW_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *gp_reply = xcb_grab_pointer_reply(conn, gp_cookie, NULL);
    if (!gp_reply || gp_reply->status != XCB_GRAB_STATUS_SUCCESS)
    {
        fprintf(stderr, "Failed to grab pointer for moving window\n");
        dragging = 0;
    }
    free(gp_reply);
}

void launch_picom()
{
    pid_t pid = fork();
    if (pid == 0)
    {
        // In the child process:
        setsid(); // Create a new session
        // Replace the child process with picom.
        // Make sure to use the correct path to picom or have it in your PATH.
        execlp("picom", "picom", "--config", "/home/serio/.config/picom.conf", NULL);
        // If execlp fails:
        fprintf(stderr, "Failed to launch picom\n");
        exit(1);
    }
    // The parent process continues.
}

/*
 * start_resize: Begins a resize operation.
 * The parameter 'flags' indicates which edges are active.
 */
void start_resize(Client *c, int pointer_x, int pointer_y, int flags)
{
    if (c->state == STATE_FULLSCREEN)
        return; /* Don't resize fullscreen windows */

    resizing = 1;
    resize_client = c;
    resize_start_x = pointer_x;
    resize_start_y = pointer_y;
    resize_flags = flags;
    xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
    if (geo)
    {
        orig_frame_x = geo->x;
        orig_frame_y = geo->y;
        orig_frame_width = geo->width;
        orig_frame_height = geo->height;
        free(geo);
    }
    xcb_grab_pointer_cookie_t gp_cookie = xcb_grab_pointer(conn, 1, screen->root,
                                                           XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                                                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                                           XCB_WINDOW_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *gp_reply = xcb_grab_pointer_reply(conn, gp_cookie, NULL);
    if (!gp_reply || gp_reply->status != XCB_GRAB_STATUS_SUCCESS)
    {
        fprintf(stderr, "Failed to grab pointer for resizing window\n");
        resizing = 0;
    }
    free(gp_reply);
}

void end_drag()
{
    dragging = 0;
    drag_client = NULL;
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
}

void end_resize()
{
    resizing = 0;
    resize_client = NULL;
    resize_flags = 0;
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
}

/*
 * update_resize: Called during motion notify while resizing.
 * Computes the new geometry based on which edges are active.
 */
void update_resize(int pointer_x, int pointer_y)
{
    int dx = pointer_x - resize_start_x;
    int dy = pointer_y - resize_start_y;
    int new_x = orig_frame_x;
    int new_y = orig_frame_y;
    int new_width = orig_frame_width;
    int new_height = orig_frame_height;

    if (resize_flags & RESIZE_LEFT)
    {
        new_x = orig_frame_x + dx;
        new_width = orig_frame_width - dx;
    }
    if (resize_flags & RESIZE_RIGHT)
    {
        new_width = orig_frame_width + dx;
    }
    if (resize_flags & RESIZE_TOP)
    {
        new_y = orig_frame_y + dy;
        new_height = orig_frame_height - dy;
    }
    if (resize_flags & RESIZE_BOTTOM)
    {
        new_height = orig_frame_height + dy;
    }

    if (new_width < MIN_WIDTH)
    {
        if (resize_flags & RESIZE_LEFT)
            new_x = orig_frame_x + (orig_frame_width - MIN_WIDTH);
        new_width = MIN_WIDTH;
    }
    if (new_height < MIN_HEIGHT)
    {
        if (resize_flags & RESIZE_TOP)
            new_y = orig_frame_y + (orig_frame_height - MIN_HEIGHT);
        new_height = MIN_HEIGHT;
    }

    uint32_t values[4] = {new_x, new_y, new_width, new_height};
    xcb_configure_window(conn, resize_client->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    values[0] = RESIZE_BORDER;
    values[1] = 0;
    values[2] = new_width - 2 * RESIZE_BORDER;
    values[3] = TITLE_BAR_HEIGHT;
    xcb_configure_window(conn, resize_client->title,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    values[0] = RESIZE_BORDER;
    values[1] = TITLE_BAR_HEIGHT;
    values[2] = new_width - 2 * RESIZE_BORDER;
    values[3] = new_height - TITLE_BAR_HEIGHT - RESIZE_BORDER;
    xcb_configure_window(conn, resize_client->client,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    set_rounded_corners(resize_client->frame, new_width, new_height, CORNER_RADIUS);
    xcb_flush(conn);
}

/* Helper function to launch xterm */
void launch_xterm()
{
    pid_t pid = fork();
    if (pid == 0)
    {
        setsid();
        execl("/usr/bin/xterm", "xterm", NULL);
        fprintf(stderr, "Failed to launch xterm\n");
        exit(1);
    }
}

/* Main event loop */
int main(void)
{
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn))
    {
        fprintf(stderr, "Cannot open display\n");
        exit(EXIT_FAILURE);
    }

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    screen = iter.data;

    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(conn, screen->root,
                                                                    XCB_CW_EVENT_MASK, &mask);
    if (xcb_request_check(conn, cookie))
    {
        fprintf(stderr, "Another window manager is already running.\n");
        xcb_disconnect(conn);
        exit(EXIT_FAILURE);
    }
    xcb_flush(conn);

    // Launch picom for compositing/translucency.
    launch_picom();

    launch_xterm();
    xcb_generic_event_t *event;

    /// Load the image using Cairo
    cairo_surface_t *bg_surface = cairo_image_surface_create_from_png("/home/serio/etyWM/background_sm.png");
    cairo_status_t status = cairo_surface_status(bg_surface);
    if (status != CAIRO_STATUS_SUCCESS)
    {
        fprintf(stderr, "Error loading PNG: %s\n", cairo_status_to_string(status));
        // Handle error (e.g., exit)
    }

    // Original image dimensions
    int img_width = cairo_image_surface_get_width(bg_surface);
    int img_height = cairo_image_surface_get_height(bg_surface);

    // Screen dimensions
    int screen_width = screen->width_in_pixels;
    int screen_height = screen->height_in_pixels;

    // Create a new Cairo surface with screen dimensions
    cairo_surface_t *scaled_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, screen_width, screen_height);
    cairo_t *cr = cairo_create(scaled_surface);

    // Calculate scale factors
    double scale_x = (double)screen_width / img_width;
    double scale_y = (double)screen_height / img_height;

    // Option 1: Scale non-uniformly (stretches the image to exactly fill the screen)
    cairo_scale(cr, scale_x, scale_y);

    // Draw the original image onto the new (scaled) surface
    cairo_set_source_surface(cr, bg_surface, 0, 0);
    cairo_paint(cr);

    // Clean up
    cairo_destroy(cr);
    cairo_surface_destroy(bg_surface); // optionally free the original surface if no longer needed

    int width = screen_width;   // use screen width
    int height = screen_height; // use screen height
    xcb_pixmap_t bg_pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, bg_pixmap, screen->root, width, height);

    // Create a graphics context and copy the image data to the pixmap
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, bg_pixmap, 0, NULL);
    int stride = cairo_image_surface_get_stride(scaled_surface);
    int data_len = stride * height;
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, bg_pixmap, gc,
                  width, height, 0, 0, 0, screen->root_depth, data_len,
                  (const char *)cairo_image_surface_get_data(scaled_surface));

    // Set the root window's background to the pixmap
    uint32_t value = bg_pixmap;
    xcb_change_window_attributes(conn, screen->root, XCB_CW_BACK_PIXMAP, &value);
    xcb_clear_area(conn, 0, screen->root, 0, 0, screen->width_in_pixels, screen->height_in_pixels);

    // Set properties for compositors:
    xcb_intern_atom_cookie_t cookie1 = xcb_intern_atom(conn, 0, strlen("_XROOTPMAP_ID"), "_XROOTPMAP_ID");
    xcb_intern_atom_reply_t *reply1 = xcb_intern_atom_reply(conn, cookie1, NULL);
    if (reply1)
    {
        xcb_change_property(conn,
                            XCB_PROP_MODE_REPLACE,
                            screen->root,
                            reply1->atom,
                            XCB_ATOM_PIXMAP,
                            32,
                            1,
                            (unsigned char *)&bg_pixmap);
        free(reply1);
    }

    xcb_intern_atom_cookie_t cookie2 = xcb_intern_atom(conn, 0, strlen("ESETROOT_PMAP_ID"), "ESETROOT_PMAP_ID");
    xcb_intern_atom_reply_t *reply2 = xcb_intern_atom_reply(conn, cookie2, NULL);
    if (reply2)
    {
        xcb_change_property(conn,
                            XCB_PROP_MODE_REPLACE,
                            screen->root,
                            reply2->atom,
                            XCB_ATOM_PIXMAP,
                            32,
                            1,
                            (unsigned char *)&bg_pixmap);
        free(reply2);
    }

    xcb_free_gc(conn, gc);
    cairo_surface_destroy(scaled_surface);

    while ((event = xcb_wait_for_event(conn)))
    {
        uint8_t response = event->response_type & ~0x80;
        switch (response)
        {
        case XCB_MAP_REQUEST:
        {
            xcb_map_request_event_t *map_req = (xcb_map_request_event_t *)event;
            create_frame(map_req->window);
            break;
        }
        case XCB_UNMAP_NOTIFY:
        {
            xcb_unmap_notify_event_t *unmap = (xcb_unmap_notify_event_t *)event;
            Client *c = find_client(unmap->window);
            if (c && unmap->window == c->client)
            {
                xcb_unmap_window(conn, c->frame);
                xcb_flush(conn);
            }
            break;
        }
        case XCB_CONFIGURE_REQUEST:
        {
            xcb_configure_request_event_t *cfg_req = (xcb_configure_request_event_t *)event;
            Client *c = find_client(cfg_req->window);

            if (c && cfg_req->window == c->client)
            {
                uint32_t mask = 0;
                uint32_t values[7];
                int i = 0;
                if (cfg_req->value_mask & XCB_CONFIG_WINDOW_WIDTH)
                {
                    mask |= XCB_CONFIG_WINDOW_WIDTH;
                    values[i++] = cfg_req->width;
                    uint32_t frame_width = cfg_req->width + 2 * RESIZE_BORDER;
                    xcb_configure_window(conn, c->frame,
                                         XCB_CONFIG_WINDOW_WIDTH, &frame_width);
                }
                if (cfg_req->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
                {
                    mask |= XCB_CONFIG_WINDOW_HEIGHT;
                    values[i++] = cfg_req->height;
                    uint32_t frame_height = cfg_req->height + TITLE_BAR_HEIGHT + RESIZE_BORDER;
                    xcb_configure_window(conn, c->frame,
                                         XCB_CONFIG_WINDOW_HEIGHT, &frame_height);
                }
                if (mask)
                    xcb_configure_window(conn, cfg_req->window, mask, values);
            }
            else
            {
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
        case XCB_BUTTON_PRESS:
        {
            xcb_button_press_event_t *bp = (xcb_button_press_event_t *)event;
            Client *c = find_client(bp->event);
            if (!c)
                c = find_client(bp->child);
            if (c)
            {
                uint32_t values[] = {XCB_STACK_MODE_ABOVE};
                xcb_configure_window(conn, c->frame,
                                     XCB_CONFIG_WINDOW_STACK_MODE, values);

                if (bp->detail == 3)
                { /* Right-click: close window */
                    destroy_client(c);
                }
                else if (bp->detail == 1)
                { /* Left-click */
                    if (bp->event == c->title)
                    {
                        if (bp->time - last_click_time < 300)
                        {
                            toggle_fullscreen(c);
                            last_click_time = 0;
                        }
                        else
                        {
                            last_click_time = bp->time;
                            start_drag(c, bp->root_x, bp->root_y);
                        }
                    }
                    else
                    {
                        int rel_x = bp->event_x;
                        int rel_y = bp->event_y;
                        xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
                        xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
                        if (!geo)
                            break;

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

                        if (flags)
                        {
                            start_resize(c, bp->root_x, bp->root_y, flags);
                        }
                    }
                }
                xcb_flush(conn);
            }
            break;
        }
        case XCB_MOTION_NOTIFY:
        {
            xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)event;
            if (dragging && drag_client)
            {
                int dx = motion->root_x - drag_start_x;
                int dy = motion->root_y - drag_start_y;
                int new_x = frame_start_x + dx;
                int new_y = frame_start_y + dy;
                uint32_t values[2] = {new_x, new_y};
                xcb_configure_window(conn, drag_client->frame,
                                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
                xcb_flush(conn);
            }
            else if (resizing && resize_client)
            {
                update_resize(motion->root_x, motion->root_y);
            }
            break;
        }
        case XCB_BUTTON_RELEASE:
        {
            if (dragging)
                end_drag();
            if (resizing)
                end_resize();
            break;
        }
        case XCB_DESTROY_NOTIFY:
        {
            xcb_destroy_notify_event_t *dn = (xcb_destroy_notify_event_t *)event;
            Client *c = find_client(dn->window);
            if (c)
            {
                if (dn->window == c->client)
                {
                    xcb_destroy_window(conn, c->frame);
                    remove_client_by_frame(c->frame);
                }
            }
            break;
        }
        default:
            break;
        }
        free(event);
    }

    xcb_disconnect(conn);
    return 0;
}

#include "client.h"
#include "config.h"
#include "draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/render.h>
#include <xcb/shape.h>

#define XCB_RENDER_PICT_FORMAT_ARGB32 0x34325241
#define XCB_RENDER_CP_ALPHA 0x00000001



static Client *clients = NULL;

void add_client(Client *c)
{
    if (!c)
    {
        fprintf(stderr, "Warning: Tried to add a NULL client\n");
        return;
    }
    c->next = clients;
    clients = c;
    fprintf(stderr, "Info: Added client (frame 0x%x)\n", c->frame);
}

void remove_client_by_frame(xcb_window_t frame)
{
    Client **curr = &clients;
    while (*curr)
    {
        if ((*curr)->frame == frame)
        {
            Client *tmp = *curr;
            *curr = (*curr)->next;
            fprintf(stderr, "Info: Removing client (frame 0x%x)\n", frame);
            free(tmp);
            return;
        }
        curr = &((*curr)->next);
    }
    fprintf(stderr, "Warning: No client found with frame 0x%x to remove\n", frame);
}

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

void apply_alpha_blending(xcb_connection_t *conn, xcb_window_t window, uint8_t alpha_value)
{
    xcb_render_picture_t picture = xcb_generate_id(conn);
    xcb_render_create_picture(conn, picture, window, XCB_RENDER_PICT_FORMAT_ARGB32, 0, NULL);

    uint32_t values[1] = {alpha_value};
    xcb_render_change_picture(conn, picture, XCB_RENDER_CP_ALPHA, values);

    xcb_render_composite(conn, XCB_RENDER_PICT_OP_OVER, picture, 0, picture, 0, 0, 0, 0, 0, 0, 0, 0);
    xcb_flush(conn);
}

void toggle_fullscreen(xcb_connection_t *conn, xcb_screen_t *screen, Client *c)
{
    if (!conn || !screen || !c)
    {
        fprintf(stderr, "Error: Invalid parameter(s) in toggle_fullscreen\n");
        return;
    }

    if (c->state == STATE_NORMAL)
    {
        xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(conn, c->frame);
        xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn, geo_cookie, NULL);
        if (geo)
        {
            c->saved_x = geo->x;
            c->saved_y = geo->y;
            c->saved_w = geo->width;
            c->saved_h = geo->height;
            free(geo);
            fprintf(stderr, "Info: Saved geometry for client (frame 0x%x)\n", c->frame);
        }
        else
        {
            fprintf(stderr, "Error: Could not get geometry for client (frame 0x%x)\n", c->frame);
            return;
        }

        uint32_t values[4] = {0, 0, screen->width_in_pixels, screen->height_in_pixels};
        xcb_configure_window(conn, c->frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        values[0] = RESIZE_BORDER;
        values[1] = TITLE_BAR_HEIGHT;
        values[2] = screen->width_in_pixels - 2 * RESIZE_BORDER;
        values[3] = screen->height_in_pixels - TITLE_BAR_HEIGHT - RESIZE_BORDER;
        xcb_configure_window(conn, c->client,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        values[0] = RESIZE_BORDER;
        values[1] = 0;
        values[2] = screen->width_in_pixels - 2 * RESIZE_BORDER;
        values[3] = TITLE_BAR_HEIGHT;
        xcb_configure_window(conn, c->title,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        c->state = STATE_FULLSCREEN;
        fprintf(stderr, "Info: Client (frame 0x%x) set to fullscreen\n", c->frame);
    }
    else
    {
        uint32_t values[4] = {c->saved_x, c->saved_y, c->saved_w, c->saved_h};
        xcb_configure_window(conn, c->frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        values[0] = RESIZE_BORDER;
        values[1] = 0;
        values[2] = c->saved_w - 2 * RESIZE_BORDER;
        values[3] = TITLE_BAR_HEIGHT;
        xcb_configure_window(conn, c->title,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             values);

        c->state = STATE_NORMAL;
        fprintf(stderr, "Info: Client (frame 0x%x) restored to normal state\n", c->frame);
    }

    /* Apply alpha blending to the client window */
    uint8_t alpha_value = 0x80; // 50% opacity
    apply_alpha_blending(conn, c->client, alpha_value);

    /* Update rounded corners for the frame */
    int frame_width = (c->state == STATE_NORMAL) ? c->saved_w : screen->width_in_pixels;
    int frame_height = (c->state == STATE_NORMAL) ? c->saved_h : screen->height_in_pixels;
    set_rounded_corners(conn, c->frame, frame_width, frame_height, CORNER_RADIUS);
    xcb_flush(conn);
}

void create_frame(xcb_connection_t *conn, xcb_screen_t *screen, xcb_window_t client)
{
    if (!conn || !screen)
    {
        fprintf(stderr, "Error: Invalid connection or screen in create_frame\n");
        return;
    }

    /* Get client geometry */
    xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, client);
    xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn, geom_cookie, NULL);
    if (!geom)
    {
        fprintf(stderr, "Error: Could not get geometry for client window 0x%x\n", client);
        return;
    }

    int frame_x = geom->x;
    int frame_y = geom->y;
    int client_width = geom->width;
    int client_height = geom->height;
    int frame_width = client_width + 2 * RESIZE_BORDER;
    int frame_height = client_height + TITLE_BAR_HEIGHT + RESIZE_BORDER;

    /* Create the frame window */
    xcb_window_t frame = xcb_generate_id(conn);
    uint32_t frame_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t frame_values[2] = {screen->white_pixel,
                                XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                                    XCB_EVENT_MASK_BUTTON_PRESS |
                                    XCB_EVENT_MASK_EXPOSURE |
                                    XCB_EVENT_MASK_POINTER_MOTION};
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, frame, screen->root,
                      frame_x, frame_y,
                      frame_width, frame_height,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual,
                      frame_mask, frame_values);
    fprintf(stderr, "Info: Created frame window 0x%x for client 0x%x\n", frame, client);

    /* Apply rounded corners to the frame */
    set_rounded_corners(conn, frame, frame_width, frame_height, CORNER_RADIUS);

    /* Create the title bar as a child of the frame */
    xcb_window_t title = xcb_generate_id(conn);
    uint32_t title_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t title_values[2] = {0xD0D0D0, /* light gray */
                                XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS};
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, title, frame,
                      RESIZE_BORDER, 0,
                      frame_width - 2 * RESIZE_BORDER, TITLE_BAR_HEIGHT,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual,
                      title_mask, title_values);

    /* Set a WM_NAME for the title bar */
    const char *title_name = "etyWM_title";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, title,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        strlen(title_name), title_name);
    fprintf(stderr, "Info: Title bar created for frame 0x%x\n", frame);

    /* Set the _NET_WM_WINDOW_OPACITY property for translucency on the title bar */
    xcb_intern_atom_cookie_t opacity_cookie = xcb_intern_atom(conn, 0,
                                                              strlen("_NET_WM_WINDOW_OPACITY"),
                                                              "_NET_WM_WINDOW_OPACITY");
    xcb_intern_atom_reply_t *opacity_reply = xcb_intern_atom_reply(conn, opacity_cookie, NULL);
    if (opacity_reply)
    {
        // Set to half transparency (approximately 50% opacity)
        uint32_t opacity = 0x7FFFFFFF;
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, title,
                            opacity_reply->atom, XCB_ATOM_CARDINAL, 32, 1, &opacity);
        free(opacity_reply);
    }
    else
    {
        fprintf(stderr, "Warning: Could not set window opacity for title bar (frame 0x%x)\n", frame);
    }

    /* Reparent the client window into the frame */
    xcb_reparent_window(conn, client, frame, RESIZE_BORDER, TITLE_BAR_HEIGHT);
    uint32_t border_width = 0;
    xcb_configure_window(conn, client, XCB_CONFIG_WINDOW_BORDER_WIDTH, &border_width);

    /* Map the client, title, and frame windows */
    xcb_map_window(conn, client);
    xcb_map_window(conn, title);
    xcb_map_window(conn, frame);
    xcb_flush(conn);
    free(geom);

    /* Allocate and add a new Client record */
    Client *c = malloc(sizeof(Client));
    if (!c)
    {
        fprintf(stderr, "Error: Out of memory when allocating Client structure\n");
        exit(EXIT_FAILURE);
    }
    c->client = client;
    c->frame = frame;
    c->title = title;
    c->state = STATE_NORMAL;
    c->next = NULL;
    add_client(c);
}

void destroy_client(xcb_connection_t *conn, Client *c)
{
    if (!conn || !c)
    {
        fprintf(stderr, "Error: Invalid parameter(s) in destroy_client\n");
        return;
    }

    fprintf(stderr, "Info: Destroying client (frame 0x%x, client 0x%x)\n", c->frame, c->client);
    xcb_kill_client(conn, c->client);
    xcb_destroy_window(conn, c->frame);
    xcb_flush(conn);
    remove_client_by_frame(c->frame);
}

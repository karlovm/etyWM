#ifndef CLIENT_H
#define CLIENT_H

#include <xcb/xcb.h>

/* Structure representing a managed client (window) */
typedef struct Client {
    xcb_window_t client;
    xcb_window_t frame;
    xcb_window_t title;
    int state;            /* STATE_NORMAL or STATE_FULLSCREEN */
    int saved_x, saved_y; /* Saved geometry for restoring from fullscreen */
    int saved_w, saved_h;
    struct Client *next;
} Client;

/* Client management functions */
void add_client(Client *c);
void remove_client_by_frame(xcb_window_t frame);
Client *find_client(xcb_window_t win);
void create_frame(xcb_connection_t *conn, xcb_screen_t *screen, xcb_window_t client);
void destroy_client(xcb_connection_t *conn, Client *c);
void toggle_fullscreen(xcb_connection_t *conn, xcb_screen_t *screen, Client *c);

#endif // CLIENT_H

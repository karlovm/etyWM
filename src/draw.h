#ifndef DRAW_H
#define DRAW_H

#include <xcb/xcb.h>

/* Draws a rounded rectangle mask on the given window.
 * The mask is applied as the shape of the window.
 */
void set_rounded_corners(xcb_connection_t *conn, xcb_window_t frame, int width, int height, int radius);

/* Loads an image from a PNG file and creates a background pixmap for the root window */
xcb_pixmap_t create_background_pixmap(xcb_connection_t *conn, xcb_screen_t *screen, const char *image_path);

#endif // DRAW_H

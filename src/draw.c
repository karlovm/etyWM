#include "draw.h"
#include "config.h"
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <xcb/xcb.h>
#include <xcb/shape.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void set_rounded_corners(xcb_connection_t *conn, xcb_window_t frame, int width, int height, int radius)
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
    cairo_arc(cr, width - r, r, r, -M_PI/2, 0);
    cairo_line_to(cr, width, height - r);
    cairo_arc(cr, width - r, height - r, r, 0, M_PI/2);
    cairo_line_to(cr, r, height);
    cairo_arc(cr, r, height - r, r, M_PI/2, M_PI);
    cairo_line_to(cr, 0, r);
    cairo_arc(cr, r, r, r, M_PI, 3*M_PI/2);
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
                  width, height, 0, 0, 0, 1,
                  data_len, (const char *)cairo_image_surface_get_data(surface));

    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, frame, 0, 0, mask_pixmap);

    xcb_free_gc(conn, gc);
    xcb_free_pixmap(conn, mask_pixmap);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

xcb_pixmap_t create_background_pixmap(xcb_connection_t *conn, xcb_screen_t *screen, const char *image_path)
{
    cairo_surface_t *bg_surface = cairo_image_surface_create_from_png(image_path);
    cairo_status_t status = cairo_surface_status(bg_surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Error loading PNG: %s\n", cairo_status_to_string(status));
        return XCB_NONE;
    }

    int img_width = cairo_image_surface_get_width(bg_surface);
    int img_height = cairo_image_surface_get_height(bg_surface);
    int screen_width = screen->width_in_pixels;
    int screen_height = screen->height_in_pixels;

    cairo_surface_t *scaled_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, screen_width, screen_height);
    cairo_t *cr = cairo_create(scaled_surface);
    double scale_x = (double)screen_width / img_width;
    double scale_y = (double)screen_height / img_height;
    cairo_scale(cr, scale_x, scale_y);
    cairo_set_source_surface(cr, bg_surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(bg_surface);

    xcb_pixmap_t bg_pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, bg_pixmap, screen->root, screen_width, screen_height);

    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, bg_pixmap, 0, NULL);
    int stride = cairo_image_surface_get_stride(scaled_surface);
    int data_len = stride * screen_height;
    xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, bg_pixmap, gc,
                  screen_width, screen_height, 0, 0, 0, screen->root_depth,
                  data_len, (const char *)cairo_image_surface_get_data(scaled_surface));

    xcb_free_gc(conn, gc);
    cairo_surface_destroy(scaled_surface);

    return bg_pixmap;
}

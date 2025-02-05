#ifndef CONFIG_H
#define CONFIG_H

/* Window layout parameters */
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
#define RESIZE_LEFT   (1 << 0)
#define RESIZE_RIGHT  (1 << 1)
#define RESIZE_TOP    (1 << 2)
#define RESIZE_BOTTOM (1 << 3)

/* Window state flags */
#define STATE_NORMAL     0
#define STATE_FULLSCREEN 1

#endif // CONFIG_H

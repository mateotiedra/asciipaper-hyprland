#ifndef ASCII_POINTER_CURSOR_TRACKER_H
#define ASCII_POINTER_CURSOR_TRACKER_H

#include <stdbool.h>

/* Normalized top-left coordinates and physical output aspect, independent
 * of which application's surface currently owns the Wayland pointer. */
struct pointer_frame {
    bool valid;
    float x, y, aspect;
};

void pointer_tracker_start(void);
void pointer_tracker_snapshot(struct pointer_frame *frame);

#endif

#ifndef ASCII_POINTER_FISH_MOTION_H
#define ASCII_POINTER_FISH_MOTION_H

#include <stdbool.h>

#define FISH_SPINE_COUNT 20

struct fish_point { float x, y; };
struct fish_frame {
    bool valid;
    struct fish_point spine[FISH_SPINE_COUNT];
    float length;
    float bounds[4];
    /* Visible nose axis, independent of the trailing neck (unit forward). */
    struct fish_point heading;
};

/* Zero-initialize once per render context. All storage is owned by the caller. */
struct fish_motion {
    bool initialized;
    bool input_valid;
    double last_time;
    double remainder;
    float aspect;
    struct fish_point input;
    struct fish_point head;
    struct fish_point spine[FISH_SPINE_COUNT];
    float heading;
    float activity;
    float phase;
};

/* now: monotonic seconds; pointer: normalized top-left coordinates; aspect:
 * output width/height. Output coordinates use screen-height units. Keep the
 * zero-initialized state between calls; invalid input hides the fish without
 * discarding its world-space joints. The nose axis is independent of the
 * trailing neck, so head pivots cannot mirror the whole body. Fixed substeps
 * make 8/24 Hz rendering agree.
 * No allocation, IPC, or changes to the wallpaper/video clock occur here. */
void fish_motion_update(struct fish_motion *state, double now, bool valid,
                        float pointer_x, float pointer_y, float aspect,
                        struct fish_frame *frame);

#endif

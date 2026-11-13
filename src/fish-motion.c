#include "fish-motion.h"

#include <math.h>
#include <string.h>

/* Screen-height units throughout. 240 Hz divides both real render rates (24/8).
 * A 25 ms distance response caps forward swimming speed at 12 height units/s.
 * Catch-up is at most 250 ms; fixed substeps still bound every movement.
 * Body length is .18, with 19 EXACT-length persistent world-space links.
 * Each follower projects its previous position onto its new parent's circle,
 * limiting link rotation to 18 rad/s. There is no hard curvature projection:
 * a stopped head may pivot independently without moving the trailing body.
 * Thus base-node speed is at most 12 + .18*18 = 15.24 height units/s;
 * with no head translation, all base positions remain exactly unchanged.
 * The head pivots at 38 rad/s (180 degrees in .083 s), stopping translation
 * for targets behind it and easing forward with a cosine-squared speed gate.
 * The rearward 1.15-cycle wave reaches .010 amplitude, follows motion with a
 * .20 s envelope, then snaps below 1e-5 to a truly stationary resting shape.
 * All loops/storage are bounded; no allocation or wall-clock/random input.
 */
#define FISH_LENGTH .18f
#define LINK_LENGTH (FISH_LENGTH / (FISH_SPINE_COUNT - 1))
#define STEP (1.0 / 240.0)
#define PI 3.14159265358979323846f
#define TAU (2.0f * PI)
#define HEAD_TURN_RATE 38.0f
#define BODY_TURN_RATE 18.0f
#define REST_DISTANCE .00001f

static float clampf(float x, float lo, float hi)
{
    return fminf(hi, fmaxf(lo, x));
}

static float angle_delta(float target, float current)
{
    return remainderf(target - current, TAU);
}

static struct fish_point point_at(struct fish_point p, float a, float distance)
{
    return (struct fish_point){p.x + cosf(a) * distance,
                               p.y + sinf(a) * distance};
}

static void initialize(struct fish_motion *s, struct fish_point target,
                       float aspect, double now)
{
    memset(s, 0, sizeof(*s));
    s->initialized = true;
    s->input_valid = true;
    s->last_time = now;
    s->aspect = aspect;
    s->input = s->head = target;
    for (unsigned i = 0; i < FISH_SPINE_COUNT; ++i)
        s->spine[i] = point_at(target, PI, LINK_LENGTH * i);
}

static void follow_head(struct fish_motion *s, bool reproject)
{
    struct fish_point old_parent = s->spine[0];
    s->spine[0] = s->head;
    for (unsigned i = 1; i < FISH_SPINE_COUNT; ++i) {
        struct fish_point old = s->spine[i];
        struct fish_point parent = s->spine[i - 1];
        /* No heading or curvature target enters this solve. In particular, a
         * stationary parent leaves its follower bit-for-bit stationary. */
        if (reproject || parent.x != old_parent.x || parent.y != old_parent.y) {
            float previous = atan2f(old.y - old_parent.y,
                                   old.x - old_parent.x);
            float dx = old.x - parent.x, dy = old.y - parent.y;
            /* At coincidence keep the old link direction rather than letting
             * atan2(0,0) pick a new side of the body. */
            float desired = hypotf(dx, dy) > .000001f ?
                            atan2f(dy, dx) : previous;
            float turn = clampf(angle_delta(desired, previous),
                                -BODY_TURN_RATE * (float)STEP,
                                 BODY_TURN_RATE * (float)STEP);
            s->spine[i] = point_at(parent, previous + turn, LINK_LENGTH);
        }
        old_parent = old;
    }
}

static void advance(struct fish_motion *s, struct fish_point target)
{
    const float dt = (float)STEP;
    const float follow = 1.0f - expf(-dt / .025f);
    float dx = target.x - s->head.x, dy = target.y - s->head.y;
    float distance = hypotf(dx, dy);
    float travel = 0.0f;
    /* Steer first, then swim only along the visible head orientation. Moving
     * directly toward the target makes a reversing fish slide tail-first. */
    if (distance >= REST_DISTANCE) {
        float target_heading = atan2f(dy, dx);
        float turn = angle_delta(target_heading, s->heading);
        s->heading = remainderf(s->heading +
                               clampf(turn, -HEAD_TURN_RATE * dt,
                                            HEAD_TURN_RATE * dt), TAU);
        float alignment = fmaxf(0.0f, cosf(angle_delta(target_heading, s->heading)));
        travel = fminf(distance * follow, 12.0f * dt) * alignment * alignment;
        s->head = point_at(s->head, s->heading, travel);
    } else if (distance > 0.0f) {
        /* Far below a logical pixel: exact finite-time rest prevents endless
         * tiny corrective turns/orbits and lets the body become stationary. */
        s->head = target;
        travel = distance;
    }
    follow_head(s, false);
    float activity_target = clampf(travel / (dt * .20f), 0.0f, 1.0f);
    s->activity += (activity_target - s->activity) * (1.0f - expf(-dt / .20f));
    if (activity_target == 0.0f && s->activity < .00001f)
        s->activity = 0.0f;
    if (s->activity > 0.0f)
        s->phase = remainderf(s->phase + TAU * 1.6f * dt, TAU);
}

static void render_frame(const struct fish_motion *s, struct fish_frame *frame)
{
    struct fish_point previous_wave = s->spine[0];
    frame->valid = true;
    frame->length = FISH_LENGTH;
    frame->heading = (struct fish_point){cosf(s->heading), sinf(s->heading)};
    frame->spine[0] = s->spine[0];
    for (unsigned i = 1; i < FISH_SPINE_COUNT; ++i) {
        struct fish_point base = s->spine[i];
        float direction = atan2f(base.y - s->spine[i - 1].y,
                                 base.x - s->spine[i - 1].x);
        float t = (float)i / (FISH_SPINE_COUNT - 1);
        /* A traveling wave perturbs the persistent body, not the head axis.
         * Its envelope decays to zero without resetting any follower. */
        float wave = i == 1 ? 0.0f :
                     .010f * s->activity * t * t * sinf(s->phase - TAU * 1.15f * t);
        struct fish_point displaced = {base.x - sinf(direction) * wave,
                                       base.y + cosf(direction) * wave};
        float dx = displaced.x - previous_wave.x, dy = displaced.y - previous_wave.y;
        float distance = hypotf(dx, dy);
        /* Normalize wave edges too: swimming never changes anatomical length. */
        if (distance > .000001f) {
            frame->spine[i].x = frame->spine[i - 1].x + dx * (LINK_LENGTH / distance);
            frame->spine[i].y = frame->spine[i - 1].y + dy * (LINK_LENGTH / distance);
        } else {
            frame->spine[i] = point_at(frame->spine[i - 1], direction, LINK_LENGTH);
        }
        previous_wave = displaced;
    }
    float xmin = frame->spine[0].x, xmax = xmin;
    float ymin = frame->spine[0].y, ymax = ymin;
    for (unsigned i = 1; i < FISH_SPINE_COUNT; ++i) {
        xmin = fminf(xmin, frame->spine[i].x);
        xmax = fmaxf(xmax, frame->spine[i].x);
        ymin = fminf(ymin, frame->spine[i].y);
        ymax = fmaxf(ymax, frame->spine[i].y);
    }
    const float margin = .035f * (FISH_LENGTH / .18f);
    frame->bounds[0] = xmin - margin;
    frame->bounds[1] = ymin - margin;
    frame->bounds[2] = xmax + margin;
    frame->bounds[3] = ymax + margin;
}

void fish_motion_update(struct fish_motion *s, double now, bool valid,
                        float pointer_x, float pointer_y, float aspect,
                        struct fish_frame *frame)
{
    memset(frame, 0, sizeof(*frame));
    if (!isfinite(now))
        return;
    valid = valid && isfinite(pointer_x) && isfinite(pointer_y)
                  && isfinite(aspect) && aspect > 0.0f && aspect <= 100.0f;
    if (!valid) {
        if (s->initialized) {
            s->last_time = now;
            s->remainder = 0.0;
            s->input_valid = false;
        }
        return;
    }
    struct fish_point target = {clampf(pointer_x, 0.0f, 1.0f) * aspect,
                                clampf(pointer_y, 0.0f, 1.0f)};
    if (!s->initialized) {
        initialize(s, target, aspect, now);
        render_frame(s, frame);
        return;
    }
    if (aspect != s->aspect) {
        float scale = aspect / s->aspect;
        s->head.x *= scale;
        s->input.x *= scale;
        for (unsigned i = 0; i < FISH_SPINE_COUNT; ++i)
            s->spine[i].x *= scale;
        s->aspect = aspect;
        /* Rescaling stored world positions must restore anatomical lengths
         * even when the head has not moved on this frame. */
        follow_head(s, true);
    }
    double elapsed = now - s->last_time;
    double consumed = fmin(fmax(elapsed, 0.0), .25);
    struct fish_point start = s->input_valid ? s->input : s->head;
    /* Timestamped endpoint interpolation makes constant-velocity input identical
     * at 8 and 24 Hz, unlike holding each newly sampled target for a full frame.
     * Stalls/invalid gaps retain the existing body, but do not replay old time. */
    if (elapsed > .25)
        start = s->head;
    double old_remainder = s->remainder;
    s->remainder += consumed;
    double time = STEP - old_remainder;
    while (s->remainder + 1e-12 >= STEP) {
        float f = consumed > 0.0 ? (float)clampf((float)(time / consumed), 0.0f, 1.0f) : 1.0f;
        struct fish_point step_target = {start.x + (target.x - start.x) * f,
                                         start.y + (target.y - start.y) * f};
        advance(s, step_target);
        s->remainder -= STEP;
        time += STEP;
    }
    if (s->remainder < 0.0 || elapsed < 0.0)
        s->remainder = 0.0;
    s->last_time = now;
    s->input = target;
    s->input_valid = true;
    render_frame(s, frame);
}

/* Standalone deterministic behavioral tests. Run only after integration:
 * cc -std=c11 -O2 -Wall -Wextra -Werror fish-motion.c fish-motion-test.c -lm -o /tmp/fish-motion-test
 * /tmp/fish-motion-test
 */
#include "fish-motion.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static float separation(struct fish_point a, struct fish_point b)
{
    return hypotf(a.x - b.x, a.y - b.y);
}

static void assert_geometry(const struct fish_frame *f)
{
    assert(f->valid);
    assert(isfinite(f->length) && f->length > 0.0f);
    assert(isfinite(f->heading.x) && isfinite(f->heading.y));
    assert(fabsf(hypotf(f->heading.x, f->heading.y) - 1.0f) < .000002f);
    for (unsigned i = 0; i < 4; ++i)
        assert(isfinite(f->bounds[i]));
    for (unsigned i = 0; i < FISH_SPINE_COUNT; ++i) {
        assert(isfinite(f->spine[i].x) && isfinite(f->spine[i].y));
        assert(f->spine[i].x >= f->bounds[0]);
        assert(f->spine[i].y >= f->bounds[1]);
        assert(f->spine[i].x <= f->bounds[2]);
        assert(f->spine[i].y <= f->bounds[3]);
        if (i > 0)
            assert(fabsf(separation(f->spine[i], f->spine[i - 1]) -
                         f->length / (FISH_SPINE_COUNT - 1)) < .000002f);
    }
}

static void convergence_and_rest(void)
{
    struct fish_motion s = {0};
    struct fish_frame f;
    fish_motion_update(&s, 0.0, true, .2f, .4f, 1.6f, &f);
    float previous = .32f;
    for (unsigned i = 1; i <= 24 * 8; ++i) {
        fish_motion_update(&s, i / 24.0, true, .8f, .4f, 1.6f, &f);
        assert_geometry(&f);
        assert(f.spine[0].x >= previous - .000001f);
        assert(f.spine[0].x <= 1.28f + .000001f);
        previous = f.spine[0].x;
    }
    assert(separation(f.spine[0], (struct fish_point){1.28f, .4f}) < .00001f);
    struct fish_frame resting = f;
    /* An expired raw pointer trail would make this fail: the whole body must
     * remain exactly stationary and full length even through prolonged idle. */
    for (unsigned i = 1; i <= 8 * 10; ++i) {
        fish_motion_update(&s, 8.0 + i / 8.0, true, .8f, .4f, 1.6f, &f);
        assert_geometry(&f);
        assert(memcmp(resting.spine, f.spine, sizeof(f.spine)) == 0);
    }
}

static void responsive_without_teleporting(void)
{
    struct fish_motion s = {0};
    struct fish_frame f;
    fish_motion_update(&s, 0.0, true, .2f, .4f, 1.6f, &f);
    fish_motion_update(&s, 1.0 / 240.0, true, .8f, .4f, 1.6f, &f);
    assert(f.spine[0].x > .32f);
    assert(f.spine[0].x < .5f);
    for (unsigned i = 2; i <= 48; ++i)
        fish_motion_update(&s, i / 240.0, true, .8f, .4f, 1.6f, &f);
    /* A normal screen-wide jump must be within a logical pixel in 200 ms,
     * rather than spending seconds approaching the pointer. */
    assert(separation(f.spine[0], (struct fish_point){1.28f, .4f}) < .001f);
}

static void forward_only_reversal(void)
{
    struct fish_motion s = {0};
    struct fish_frame f, previous;
    fish_motion_update(&s, 0.0, true, .2f, .5f, 1.6f, &f);
    /* Establish a visibly rightward-moving fish before the full reversal.
     * The visible nose pivots independently of the persistent trailing neck. */
    for (unsigned i = 1; i <= 120; ++i)
        fish_motion_update(&s, i / 240.0, true,
                           .2f + .7f * (float)i / 120.0f, .5f, 1.6f, &f);
    assert(f.spine[0].x > .6f);
    assert(f.heading.x > .95f);
    bool turned = false;
    for (unsigned i = 1; i <= 240 * 8; ++i) {
        previous = f;
        fish_motion_update(&s, .5 + i / 240.0, true, .1f, .5f, 1.6f, &f);
        assert_geometry(&f);
        float dx = f.spine[0].x - previous.spine[0].x;
        float dy = f.spine[0].y - previous.spine[0].y;
        float forward_x = f.heading.x, forward_y = f.heading.y;
        float old_x = previous.heading.x, old_y = previous.heading.y;
        /* Test actual integration substeps: a coarse 8 Hz chord can point
         * backwards across a rapid U-turn even when each step is forward.
         * Only the documented subpixel final snap may point backwards. */
        assert(dx * forward_x + dy * forward_y >= -.000011f);
        assert(dx * old_x + dy * old_y >= -.000011f);
        /* A fast reversal still cannot teleport any part of the body. This
         * includes 12 height/s translation and the full chain's bounded turn. */
        for (unsigned j = 1; j < FISH_SPINE_COUNT; ++j)
            assert(separation(f.spine[j], previous.spine[j]) < .065f);
        if (i <= 24 && forward_x < -.95f)
            turned = true;
        /* The first 90 degrees of a behind-target pivot must not translate. */
        if (forward_x > .1f)
            assert(hypotf(dx, dy) < .000001f);
        if (i == 96)
            assert(separation(f.spine[0], (struct fish_point){.16f, .5f}) < .001f);
    }
    assert(turned);
    assert(separation(f.spine[0], (struct fish_point){.16f, .5f}) < .00001f);
    struct fish_frame resting = f;
    for (unsigned i = 1; i <= 8 * 10; ++i) {
        fish_motion_update(&s, 8.5 + i / 8.0, true, .1f, .5f, 1.6f, &f);
        assert_geometry(&f);
        assert(memcmp(resting.spine, f.spine, sizeof(f.spine)) == 0);
    }
}

static void close_head_crossing_preserves_tail(void)
{
    struct fish_motion s = {0};
    struct fish_frame f, previous, initial;
    fish_motion_update(&s, 0.0, true, .5f, .5f, 1.0f, &f);
    initial = f;
    float tail_travel = 0.0f;
    for (unsigned i = 1; i <= 120; ++i) {
        previous = f;
        /* Three height-normalized pixels on a 1000-pixel output, with tiny
         * alternating offsets across the +/-pi steering seam. */
        float y = .5f + (i % 2 ? .00002f : -.00002f);
        fish_motion_update(&s, i / 240.0, true, .497f, y, 1.0f, &f);
        assert_geometry(&f);
        float turn = atan2f(previous.heading.x * f.heading.y -
                           previous.heading.y * f.heading.x,
                           previous.heading.x * f.heading.x +
                           previous.heading.y * f.heading.y);
        assert(fabsf(turn) <= 38.0f / 240.0f + .000002f);
        for (unsigned j = 1; j < FISH_SPINE_COUNT; ++j) {
            assert(separation(f.spine[j], previous.spine[j]) < .004f);
            assert(separation(f.spine[j], initial.spine[j]) < .025f);
        }
        /* Before the nose has turned 90 degrees the head is stopped. Its
         * independent pivot must leave every body point at its old location. */
        if (i <= 9) {
            assert(separation(f.spine[0], initial.spine[0]) < .0000001f);
            assert(memcmp(f.spine, initial.spine, sizeof(f.spine)) == 0);
        }
        tail_travel += separation(f.spine[FISH_SPINE_COUNT - 1],
                                  previous.spine[FISH_SPINE_COUNT - 1]);
    }
    /* Repeated seam crossings cannot rack up high-frequency tail sweeps. */
    assert(tail_travel < .04f);
    struct fish_point stop = f.spine[0];
    previous = f;
    /* A timestamp-only input update stops the head without replaying the
     * previous target during interpolation. Only the residual wave may fade. */
    fish_motion_update(&s, .5, true, stop.x, stop.y, 1.0f, &f);
    for (unsigned i = 1; i <= 240 * 4; ++i) {
        fish_motion_update(&s, .5 + i / 240.0, true,
                           stop.x, stop.y, 1.0f, &f);
        assert_geometry(&f);
        assert(separation(f.spine[0], stop) < .0000001f);
        assert(separation(f.spine[FISH_SPINE_COUNT - 1],
                          previous.spine[FISH_SPINE_COUNT - 1]) < .012f);
    }
    previous = f;
    fish_motion_update(&s, 4.625, true, stop.x, stop.y, 1.0f, &f);
    assert(memcmp(f.spine, previous.spine, sizeof(f.spine)) == 0);
}

static void resized_stationary_body_keeps_length(void)
{
    struct fish_motion s = {0};
    struct fish_frame frame;
    fish_motion_update(&s, 0.0, true, .5f, .5f, 1.6f, &frame);
    fish_motion_update(&s, 0.0, true, .5f, .5f, 1.0f, &frame);
    assert_geometry(&frame);
    assert(separation(frame.spine[0], (struct fish_point){.5f, .5f}) < .000001f);
}

static void turns_stalls_and_restart(void)
{
    struct fish_motion s = {0};
    struct fish_frame f, previous;
    fish_motion_update(&s, 0.0, true, .5f, .5f, 1.6f, &previous);
    for (unsigned i = 1; i <= 24 * 6; ++i) {
        unsigned leg = (i / 12) % 4;
        float x = leg == 0 ? .95f : leg == 2 ? .05f : .5f;
        float y = leg == 1 ? .95f : leg == 3 ? .05f : .5f;
        fish_motion_update(&s, i / 24.0, true, x, y, 1.6f, &f);
        assert_geometry(&f);
    }
    /* A long gap must behave like a bounded catch-up starting at the head,
     * not integrate the entire minute or teleport directly to the pointer. */
    struct fish_motion bounded = s;
    struct fish_frame expected;
    bounded.input_valid = false;
    fish_motion_update(&bounded, bounded.last_time + .25, true,
                       1.0f, 1.0f, 1.6f, &expected);
    fish_motion_update(&s, 66.0, true, 1.0f, 1.0f, 1.6f, &f);
    assert_geometry(&f);
    for (unsigned j = 0; j < FISH_SPINE_COUNT; ++j)
        assert(separation(f.spine[j], expected.spine[j]) < .00001f);
    fish_motion_update(&s, 67.0, false, NAN, NAN, 1.6f, &f);
    assert(!f.valid);
    bounded = s;
    bounded.input_valid = false;
    fish_motion_update(&bounded, bounded.last_time + .25, true,
                       0.0f, 0.0f, 1.6f, &expected);
    fish_motion_update(&s, 68.0, true, 0.0f, 0.0f, 1.6f, &f);
    assert_geometry(&f);
    for (unsigned j = 0; j < FISH_SPINE_COUNT; ++j)
        assert(separation(f.spine[j], expected.spine[j]) < .00001f);
    /* A duplicate timestamp cannot advance or reinitialize the tail. */
    previous = f;
    fish_motion_update(&s, 68.0, true, .9f, .1f, 1.6f, &f);
    assert(memcmp(previous.spine, f.spine, sizeof(f.spine)) == 0);
}

static struct fish_point trajectory(double t)
{
    if (t <= 1.0)
        return (struct fish_point){.2f + .6f * (float)t, .3f};
    if (t <= 2.0)
        return (struct fish_point){.8f, .3f + .4f * (float)(t - 1.0)};
    return (struct fish_point){.8f, .7f};
}

static void run_rate(unsigned hz, struct fish_frame output[33])
{
    struct fish_motion s = {0};
    struct fish_frame frame;
    for (unsigned i = 0; i <= hz * 4; ++i) {
        double now = (double)i / hz;
        struct fish_point target = trajectory(now);
        fish_motion_update(&s, now, true, target.x, target.y, 1.6f, &frame);
        assert_geometry(&frame);
        if (i % (hz / 8) == 0)
            output[i / (hz / 8)] = frame;
    }
}

static void frame_rate_consistency(void)
{
    struct fish_frame fast[33], idle[33];
    run_rate(24, fast);
    run_rate(8, idle);
    for (unsigned i = 0; i < 33; ++i)
        for (unsigned j = 0; j < FISH_SPINE_COUNT; ++j)
            assert(separation(fast[i].spine[j], idle[i].spine[j]) < .001f);
}

int main(void)
{
    convergence_and_rest();
    responsive_without_teleporting();
    forward_only_reversal();
    close_head_crossing_preserves_tail();
    resized_stationary_body_keeps_length();
    turns_stalls_and_restart();
    frame_rate_consistency();
    puts("fish-motion: responsive pursuit, convergence, rest, forward-only reversal, close head crossing, turns, stalls, restart and 8/24 Hz consistency passed");
    return 0;
}

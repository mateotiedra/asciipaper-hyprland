#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <wayland-client.h>
#include "cursor-tracker.h"
#include "fish-motion.h"

/* Local adapter for the native wallpaper's single desktop output, thread,
 * GL context and seat. Every pointer callback keeps its original userdata and
 * arguments. Enter forwards first, then hides only this application's cursor.
 * Global IPC coordinates keep the swimming fish visible over applications;
 * native export mode instead uses renderer pointer, aspect and scene time.
 * No keyboard input, passwords or clicks are captured. Native frame caps and
 * all media, time and color uniforms remain unchanged.
 * Build against the installed Wayland and GLES headers:
 * cc -std=c11 -O2 -fPIC -shared -Wall -Wextra -Wpedantic hide-cursor.c \
 *    cursor-tracker.c fish-motion.c -pthread -o hide-cursor.so \
 *    $(pkg-config --cflags --libs wayland-client glesv2) -ldl -lm
 */
static int (*next_wl_proxy_add_listener)(struct wl_proxy *, void (**)(void), void *);
static void (GL_APIENTRY *next_glUseProgram)(GLuint);
static GLint (GL_APIENTRY *next_glGetUniformLocation)(GLuint, const GLchar *);
static void (GL_APIENTRY *next_glDeleteProgram)(GLuint);
static void (GL_APIENTRY *next_glUniform2f)(GLint, GLfloat, GLfloat);
static void (GL_APIENTRY *next_glUniform2fv)(GLint, GLsizei, const GLfloat *);
static void (GL_APIENTRY *next_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void (GL_APIENTRY *next_glUniform1f)(GLint, GLfloat);
enum fish_mode { MODE_UNSET, MODE_GLOBAL, MODE_NATIVE };
static enum fish_mode mode;
static bool tracker_started;
static GLuint current_program;
/* Uniform locations and native motion belong to a GL program, not a location
 * number: different programs can reuse the same numeric locations. */
struct cell_state {
    GLuint program;
    GLint pointer_location, aspect_location, time_location, strength_location;
    GLint spine_location, heading_location, bounds_location;
    GLint length_location, visible_location;
    bool have_pointer, have_aspect, have_time, stepped;
    double last_time;
    GLfloat scene_time;
    struct pointer_frame frame;
    struct fish_motion motion;
    struct fish_frame fish;
    struct cell_state *next;
};
static struct cell_state *cells;
static struct cell_state *current_cell;

_Static_assert(sizeof(struct fish_point) == 2 * sizeof(GLfloat),
               "Fish spine must contain packed XY float pairs");
static struct wl_pointer_listener listener;
static void (*original_enter)(void *, struct wl_pointer *, uint32_t,
                              struct wl_surface *, wl_fixed_t, wl_fixed_t);
static void (*original_leave)(void *, struct wl_pointer *, uint32_t,
                              struct wl_surface *);

/* Resolve and require each symbol only when its entry point is invoked. */
static void *required_symbol(const char *name) {
    dlerror();
    void *symbol = dlsym(RTLD_NEXT, name);
    if (!symbol) {
        const char *error = dlerror();
        fprintf(stderr, "ASCII cursor: cannot resolve %s: %s\n", name,
                error ? error : "symbol unavailable");
        _exit(127);
    }
    return symbol;
}

#define RESOLVE_NEXT(function) do { \
    if (!next_##function) { \
        void *symbol = required_symbol(#function); \
        _Static_assert(sizeof next_##function == sizeof symbol, \
                       "POSIX function and object pointer sizes differ"); \
        memcpy(&next_##function, &symbol, sizeof symbol); \
    } \
} while (0)

static enum fish_mode fish_mode(void) {
    if (mode == MODE_UNSET) {
        const char *value = getenv("ASCIIPAPER_FISH_MODE");
        if (!value || strcmp(value, "global") == 0)
            mode = MODE_GLOBAL;
        else if (strcmp(value, "native") == 0)
            mode = MODE_NATIVE;
        else {
            fprintf(stderr, "ASCII cursor: invalid ASCIIPAPER_FISH_MODE '%s'"
                    " (expected global or native)\n", value);
            _exit(127);
        }
    }
    return mode;
}

static struct cell_state *find_cell(GLuint program) {
    for (struct cell_state *cell = cells; cell; cell = cell->next)
        if (cell->program == program)
            return cell;
    return NULL;
}

static struct cell_state *register_cell(GLuint program) {
    struct cell_state *cell = find_cell(program);
    if (cell)
        return cell;
    cell = calloc(1, sizeof *cell);
    if (!cell) {
        fputs("ASCII cursor: cannot allocate program state\n", stderr);
        _exit(127);
    }
    cell->program = program;
    /* Query only at program discovery, never on the uniform upload path. */
    cell->pointer_location = next_glGetUniformLocation(program, "u_pointer");
    cell->aspect_location = next_glGetUniformLocation(program, "u_aspect");
    cell->time_location = next_glGetUniformLocation(program, "u_time");
    cell->strength_location = next_glGetUniformLocation(program, "u_strength");
    cell->spine_location = next_glGetUniformLocation(program, "fishSpine[0]");
    cell->heading_location = next_glGetUniformLocation(program, "fishHeading");
    cell->bounds_location = next_glGetUniformLocation(program, "fishBounds");
    cell->length_location = next_glGetUniformLocation(program, "fishLength");
    cell->visible_location = next_glGetUniformLocation(program, "fishVisible");
    cell->next = cells;
    cells = cell;
    if (current_program == program)
        current_cell = cell;
    return cell;
}

GL_APICALL void GL_APIENTRY glUseProgram(GLuint program) {
    RESOLVE_NEXT(glUseProgram);
    next_glUseProgram(program);
    current_program = program;
    current_cell = find_cell(program);
}

GL_APICALL void GL_APIENTRY glDeleteProgram(GLuint program) {
    RESOLVE_NEXT(glDeleteProgram);
    struct cell_state **link = &cells;
    while (*link) {
        struct cell_state *cell = *link;
        if (cell->program == program) {
            *link = cell->next;
            if (current_cell == cell)
                current_cell = NULL;
            free(cell);
            break;
        }
        link = &cell->next;
    }
    next_glDeleteProgram(program);
}

GL_APICALL GLint GL_APIENTRY glGetUniformLocation(GLuint program,
                                                 const GLchar *name) {
    RESOLVE_NEXT(glGetUniformLocation);
    GLint location = next_glGetUniformLocation(program, name);
    enum fish_mode selected = fish_mode();
    /* The renderer uploads time/aspect before its first pointer lookup.
     * Discover the program on that first input, so frame zero is complete. */
    if (selected == MODE_NATIVE && name &&
        (strcmp(name, "u_time") == 0 || strcmp(name, "u_aspect") == 0 ||
         strcmp(name, "u_strength") == 0))
        register_cell(program);
    if (name && strcmp(name, "u_pointer") == 0) {
        struct cell_state *cell = register_cell(program);
        if (selected == MODE_GLOBAL) {
            if (!tracker_started) {
                pointer_tracker_start();
                tracker_started = true;
            }
            /* Preserve desktop motion's real clock and off-thread IPC. */
            pointer_tracker_snapshot(&cell->frame);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            fish_motion_update(&cell->motion, now.tv_sec + now.tv_nsec * 1e-9,
                               cell->frame.valid, cell->frame.x, cell->frame.y,
                               cell->frame.aspect, &cell->fish);
        }
    }
    return location;
}

GL_APICALL void GL_APIENTRY glUniform2f(GLint location, GLfloat x, GLfloat y) {
    RESOLVE_NEXT(glUniform2f);
    struct cell_state *cell = current_cell;
    if (cell && cell->pointer_location >= 0 &&
        location == cell->pointer_location) {
        if (fish_mode() == MODE_NATIVE) {
            cell->frame.x = x;
            cell->frame.y = y;
            cell->have_pointer = true;
        } else if (cell->frame.valid) {
            x = cell->frame.x;
            y = cell->frame.y;
        }
    }
    next_glUniform2f(location, x, y);
}

GL_APICALL void GL_APIENTRY glUniform1f(GLint location, GLfloat value) {
    RESOLVE_NEXT(glUniform1f);
    struct cell_state *cell = current_cell;
    if (cell && location >= 0) {
        if (fish_mode() == MODE_NATIVE) {
            if (location == cell->aspect_location) {
                cell->frame.aspect = value;
                cell->have_aspect = true;
            } else if (location == cell->time_location) {
                cell->scene_time = value;
                cell->have_time = true;
            }
        }
        if (location == cell->strength_location) {
            if (mode == MODE_NATIVE) {
                cell->frame.valid = cell->have_pointer && cell->have_aspect &&
                                    cell->have_time && cell->frame.x >= 0.0f &&
                                    cell->frame.y >= 0.0f;
                /* Negative renderer coordinates mean no pointer. Still feed
                 * invalid samples to motion so a visible fish is cleared and
                 * retained joints resume correctly when the pointer returns. */
                if (cell->have_time) {
                    if (cell->stepped && cell->scene_time < cell->last_time) {
                        memset(&cell->motion, 0, sizeof cell->motion);
                        memset(&cell->fish, 0, sizeof cell->fish);
                    }
                    fish_motion_update(&cell->motion, cell->scene_time,
                                       cell->frame.valid,
                                       cell->frame.x, cell->frame.y,
                                       cell->frame.aspect, &cell->fish);
                    cell->last_time = cell->scene_time;
                    cell->stepped = true;
                }
            }
            if (!cell->frame.valid)
                value = 0.0f;
            if (cell->spine_location >= 0 && cell->fish.valid) {
                RESOLVE_NEXT(glUniform2fv);
                next_glUniform2fv(cell->spine_location, FISH_SPINE_COUNT,
                                  &cell->fish.spine[0].x);
            }
            if (cell->heading_location >= 0 && cell->fish.valid) {
                RESOLVE_NEXT(glUniform2f);
                next_glUniform2f(cell->heading_location, cell->fish.heading.x,
                                 cell->fish.heading.y);
            }
            if (cell->bounds_location >= 0 && cell->fish.valid) {
                RESOLVE_NEXT(glUniform4f);
                next_glUniform4f(cell->bounds_location, cell->fish.bounds[0],
                                 cell->fish.bounds[1], cell->fish.bounds[2],
                                 cell->fish.bounds[3]);
            }
            if (cell->length_location >= 0)
                next_glUniform1f(cell->length_location, cell->fish.length);
            if (cell->visible_location >= 0)
                next_glUniform1f(cell->visible_location,
                                 cell->fish.valid && value > 0.0f ? 1.0f : 0.0f);
        }
    }
    next_glUniform1f(location, value);
}

static void hide_on_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    original_enter(data, pointer, serial, surface, x, y);
    /* NULL is the standard Wayland request for no cursor image. */
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
}

static void clear_on_leave(void *data, struct wl_pointer *pointer,
                           uint32_t serial, struct wl_surface *surface) {
    if (original_leave)
        original_leave(data, pointer, serial, surface);
}

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void),
                          void *data) {
    RESOLVE_NEXT(wl_proxy_add_listener);
    if (strcmp(wl_proxy_get_class(proxy), "wl_pointer") != 0)
        return next_wl_proxy_add_listener(proxy, implementation, data);

    memcpy(&listener, implementation, sizeof listener);
    original_enter = listener.enter;
    original_leave = listener.leave;
    if (!original_enter) {
        fputs("ASCII cursor: wallpaper pointer has no enter callback\n", stderr);
        _exit(127);
    }
    listener.enter = hide_on_enter;
    listener.leave = clear_on_leave;
    return next_wl_proxy_add_listener(proxy, (void (**)(void)) &listener, data);
}

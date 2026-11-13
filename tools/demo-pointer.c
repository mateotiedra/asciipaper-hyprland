#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Export-only input driver. Preload BEFORE fish-pointer.so: only the native
 * renderer's pointer input is replaced. The shipped interposer and motion
 * model still compute every fish joint, heading, bounds and visibility. No
 * Wayland connection, real pointer movement or desktop capture is involved.
 * The pinned renderer uploads u_time before u_pointer on every frame. */
static GLuint current_program;
static GLuint scene_program;
static int scene_known;
static GLint time_location = -1;
static GLint pointer_location = -1;
static double scene_time;
static GLint (GL_APIENTRY *next_glGetUniformLocation)(GLuint, const GLchar *);
static void (GL_APIENTRY *next_glUseProgram)(GLuint);
static void (GL_APIENTRY *next_glDeleteProgram)(GLuint);
static void (GL_APIENTRY *next_glUniform1f)(GLint, GLfloat);
static void (GL_APIENTRY *next_glUniform2f)(GLint, GLfloat, GLfloat);

static void *required_symbol(const char *name) {
    dlerror();
    void *symbol = dlsym(RTLD_NEXT, name);
    if (!symbol) {
        fprintf(stderr, "demo pointer: cannot resolve %s: %s\n", name, dlerror());
        exit(127);
    }
    return symbol;
}

#define RESOLVE(function) do { \
    if (!next_##function) { \
        void *symbol = required_symbol(#function); \
        _Static_assert(sizeof next_##function == sizeof symbol, \
                       "POSIX function pointer size mismatch"); \
        memcpy(&next_##function, &symbol, sizeof symbol); \
    } \
} while (0)

GL_APICALL void GL_APIENTRY glUseProgram(GLuint program) {
    RESOLVE(glUseProgram);
    current_program = program;
    next_glUseProgram(program);
}

GL_APICALL void GL_APIENTRY glDeleteProgram(GLuint program) {
    RESOLVE(glDeleteProgram);
    if (scene_known && scene_program == program) {
        scene_known = 0;
        time_location = pointer_location = -1;
        scene_time = 0;
    }
    next_glDeleteProgram(program);
}

GL_APICALL GLint GL_APIENTRY glGetUniformLocation(GLuint program,
                                                 const GLchar *name) {
    RESOLVE(glGetUniformLocation);
    GLint location = next_glGetUniformLocation(program, name);
    if (name && strcmp(name, "u_time") == 0) {
        if (!scene_known || scene_program != program) {
            pointer_location = -1;
            scene_time = 0;
        }
        scene_program = program;
        scene_known = 1;
        time_location = location;
    } else if (name && strcmp(name, "u_pointer") == 0 &&
               scene_known && scene_program == program) {
        pointer_location = location;
    }
    return location;
}

GL_APICALL void GL_APIENTRY glUniform1f(GLint location, GLfloat value) {
    RESOLVE(glUniform1f);
    if (scene_known && current_program == scene_program &&
        time_location >= 0 && location == time_location)
        scene_time = value;
    next_glUniform1f(location, value);
}

GL_APICALL void GL_APIENTRY glUniform2f(GLint location, GLfloat x, GLfloat y) {
    RESOLVE(glUniform2f);
    if (scene_known && current_program == scene_program &&
        pointer_location >= 0 && location == pointer_location) {
        /* Smooth figure-eight with reversals and retained-tail turns. The
         * eight-second period is scene time, not wall-clock rendering time. */
        double phase = scene_time * (6.28318530717958647692 / 8.0);
        x = (GLfloat)(0.5 + 0.29 * sin(phase));
        y = (GLfloat)(0.54 + 0.19 * sin(2.0 * phase));
    }
    next_glUniform2f(location, x, y);
}

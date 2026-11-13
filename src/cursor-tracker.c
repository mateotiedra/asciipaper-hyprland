#define _GNU_SOURCE
#include "cursor-tracker.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define POLL_NANOSECONDS 41666667L
#define MONITOR_REFRESH_SECONDS 2.0
#define IPC_TIMEOUT_SECONDS 0.1
#define JSON_MAX_DEPTH 32

struct monitor {
    double x, y, width, height, scale;
};

struct tracker_state {
    bool valid;
    double x, y;
    struct monitor monitor;
};

struct json_reader {
    const char *p, *end;
};

static pthread_once_t start_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct tracker_state published;
/* Only the worker touches these buffers and its working state. */
static struct tracker_state working;
static char monitor_reply[65536];
static char cursor_reply[256];
static char ipc_error[256];
static bool unavailable_reported;

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void report_unavailable(const char *reason)
{
    if (unavailable_reported)
        return;
    unavailable_reported = true;
    fprintf(stderr, "[ascii-pointer] Global fish interaction unavailable: %s. "
            "Requires exactly one active Hyprland output and reachable "
            "$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock; "
            "retrying automatically.\n", reason);
}

static bool ipc_failure(const char *operation)
{
    int error = errno;
    snprintf(ipc_error, sizeof(ipc_error), "%s: %s", operation, strerror(error));
    return false;
}

static bool wait_socket(int fd, short events, double deadline)
{
    for (;;) {
        double remaining = deadline - monotonic_seconds();
        if (remaining <= 0.0) {
            errno = ETIMEDOUT;
            return ipc_failure("Hyprland IPC timeout");
        }
        struct pollfd descriptor = { .fd = fd, .events = events };
        int milliseconds = (int)ceil(remaining * 1000.0);
        int result = poll(&descriptor, 1, milliseconds);
        if (result > 0) {
            if (descriptor.revents & (events | POLLHUP | POLLERR))
                return true;
            errno = EBADF;
            return ipc_failure("poll Hyprland IPC");
        }
        if (result == 0) {
            errno = ETIMEDOUT;
            return ipc_failure("Hyprland IPC timeout");
        }
        if (errno != EINTR)
            return ipc_failure("poll Hyprland IPC");
    }
}

static bool ipc_request(const char *command, char *reply, size_t capacity)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !*runtime || !signature || !*signature) {
        snprintf(ipc_error, sizeof(ipc_error),
                 "XDG_RUNTIME_DIR or HYPRLAND_INSTANCE_SIGNATURE is unset");
        return false;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int length = snprintf(address.sun_path, sizeof(address.sun_path),
                          "%s/hypr/%s/.socket.sock", runtime, signature);
    if (length < 0 || (size_t)length >= sizeof(address.sun_path)) {
        snprintf(ipc_error, sizeof(ipc_error), "Hyprland IPC socket path is too long");
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return ipc_failure("create Hyprland IPC socket");
    bool success = false;
    double deadline = monotonic_seconds() + IPC_TIMEOUT_SECONDS;
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        if (errno != EINPROGRESS) {
            ipc_failure("connect Hyprland IPC socket");
            goto done;
        }
        if (!wait_socket(fd, POLLOUT, deadline))
            goto done;
        int error = 0;
        socklen_t error_length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) < 0) {
            ipc_failure("check Hyprland IPC connection");
            goto done;
        }
        if (error) {
            errno = error;
            ipc_failure("connect Hyprland IPC socket");
            goto done;
        }
    }
    size_t sent = 0, command_length = strlen(command);
    while (sent < command_length) {
        if (!wait_socket(fd, POLLOUT, deadline))
            goto done;
        ssize_t amount = send(fd, command + sent, command_length - sent, MSG_NOSIGNAL);
        if (amount > 0) {
            sent += (size_t)amount;
        } else if (amount == 0) {
            snprintf(ipc_error, sizeof(ipc_error), "Hyprland IPC closed during request");
            goto done;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            ipc_failure("send Hyprland IPC request");
            goto done;
        }
    }
    size_t used = 0;
    for (;;) {
        if (used == capacity - 1) {
            snprintf(ipc_error, sizeof(ipc_error), "Hyprland IPC reply exceeds buffer capacity");
            goto done;
        }
        if (!wait_socket(fd, POLLIN, deadline))
            goto done;
        ssize_t amount = recv(fd, reply + used, capacity - 1 - used, 0);
        if (amount > 0) {
            used += (size_t)amount;
        } else if (amount == 0) {
            reply[used] = '\0';
            success = true;
            break;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            ipc_failure("receive Hyprland IPC reply");
            goto done;
        }
    }
done:
    close(fd);
    return success;
}

static void json_space(struct json_reader *reader)
{
    while (reader->p < reader->end &&
           (*reader->p == ' ' || *reader->p == '\n' ||
            *reader->p == '\r' || *reader->p == '\t'))
        ++reader->p;
}

static bool json_take(struct json_reader *reader, char token)
{
    json_space(reader);
    if (reader->p == reader->end || *reader->p != token)
        return false;
    ++reader->p;
    return true;
}

/* Return the raw key span; escaped or nested keys never match simple field names. */
static bool json_string(struct json_reader *reader, const char **text, size_t *length)
{
    if (!json_take(reader, '"'))
        return false;
    const char *start = reader->p;
    while (reader->p < reader->end) {
        unsigned char c = (unsigned char)*reader->p++;
        if (c == '"') {
            if (text)
                *text = start;
            if (length)
                *length = (size_t)(reader->p - start - 1);
            return true;
        }
        if (c < 0x20)
            return false;
        if (c != '\\')
            continue;
        if (reader->p == reader->end)
            return false;
        char escape = *reader->p++;
        if (escape == 'u') {
            for (unsigned i = 0; i < 4; ++i) {
                if (reader->p == reader->end)
                    return false;
                char hex = *reader->p++;
                if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') ||
                      (hex >= 'A' && hex <= 'F')))
                    return false;
            }
        } else if (!strchr("\"\\/bfnrt", escape)) {
            return false;
        }
    }
    return false;
}

static bool json_number(struct json_reader *reader, double *value)
{
    json_space(reader);
    const char *start = reader->p;
    if (reader->p < reader->end && *reader->p == '-')
        ++reader->p;
    if (reader->p == reader->end)
        return false;
    if (*reader->p == '0') {
        ++reader->p;
    } else {
        if (*reader->p < '1' || *reader->p > '9')
            return false;
        do {
            ++reader->p;
        } while (reader->p < reader->end && *reader->p >= '0' && *reader->p <= '9');
    }
    if (reader->p < reader->end && *reader->p == '.') {
        ++reader->p;
        const char *digits = reader->p;
        while (reader->p < reader->end && *reader->p >= '0' && *reader->p <= '9')
            ++reader->p;
        if (reader->p == digits)
            return false;
    }
    if (reader->p < reader->end && (*reader->p == 'e' || *reader->p == 'E')) {
        ++reader->p;
        if (reader->p < reader->end && (*reader->p == '+' || *reader->p == '-'))
            ++reader->p;
        const char *digits = reader->p;
        while (reader->p < reader->end && *reader->p >= '0' && *reader->p <= '9')
            ++reader->p;
        if (reader->p == digits)
            return false;
    }
    if (value) {
        char *end;
        errno = 0;
        double number = strtod(start, &end);
        if (end != reader->p || errno == ERANGE || !isfinite(number))
            return false;
        *value = number;
    }
    return true;
}

static bool json_skip(struct json_reader *reader, unsigned depth)
{
    json_space(reader);
    if (depth > JSON_MAX_DEPTH || reader->p == reader->end)
        return false;
    char token = *reader->p;
    if (token == '"')
        return json_string(reader, NULL, NULL);
    if (token == '{' || token == '[') {
        ++reader->p;
        char close = token == '{' ? '}' : ']';
        if (json_take(reader, close))
            return true;
        do {
            if (token == '{' &&
                (!json_string(reader, NULL, NULL) || !json_take(reader, ':')))
                return false;
            if (!json_skip(reader, depth + 1))
                return false;
            if (json_take(reader, close))
                return true;
        } while (json_take(reader, ','));
        return false;
    }
    const char *literal = token == 't' ? "true" : token == 'f' ? "false" :
                          token == 'n' ? "null" : NULL;
    if (literal) {
        size_t length = strlen(literal);
        if ((size_t)(reader->end - reader->p) < length ||
            memcmp(reader->p, literal, length) != 0)
            return false;
        reader->p += length;
        return true;
    }
    return json_number(reader, NULL);
}

/* Parse only direct numeric members; nested workspace coordinates are skipped. */
static bool json_fields(struct json_reader *reader, const char *const *names,
                        double *values, unsigned field_count)
{
    if (!json_take(reader, '{'))
        return false;
    unsigned seen = 0;
    if (json_take(reader, '}'))
        return false;
    do {
        const char *key;
        size_t length;
        if (!json_string(reader, &key, &length) || !json_take(reader, ':'))
            return false;
        unsigned field = 0;
        while (field < field_count &&
               (strlen(names[field]) != length || memcmp(key, names[field], length) != 0))
            ++field;
        if (field < field_count) {
            unsigned bit = 1U << field;
            if ((seen & bit) || !json_number(reader, &values[field]))
                return false;
            seen |= bit;
        } else if (!json_skip(reader, 1)) {
            return false;
        }
        if (json_take(reader, '}'))
            return seen == (1U << field_count) - 1;
    } while (json_take(reader, ','));
    return false;
}

static bool parse_cursor(double *x, double *y)
{
    static const char *const fields[] = { "x", "y" };
    double values[2];
    struct json_reader reader = { cursor_reply, cursor_reply + strlen(cursor_reply) };
    if (!json_fields(&reader, fields, values, 2))
        return false;
    json_space(&reader);
    if (reader.p != reader.end)
        return false;
    *x = values[0];
    *y = values[1];
    return true;
}

static bool refresh_monitor(struct monitor *monitor, const char **reason)
{
    if (!ipc_request("j/monitors", monitor_reply, sizeof(monitor_reply))) {
        *reason = ipc_error;
        return false;
    }
    static const char *const fields[] = { "x", "y", "width", "height", "scale" };
    double values[5];
    struct json_reader reader = { monitor_reply, monitor_reply + strlen(monitor_reply) };
    *reason = "invalid j/monitors JSON or missing/duplicate top-level monitor geometry";
    if (!json_take(&reader, '['))
        return false;
    if (json_take(&reader, ']')) {
        *reason = "j/monitors reports no active output";
        return false;
    }
    if (!json_fields(&reader, fields, values, 5))
        return false;
    if (json_take(&reader, ',')) {
        *reason = "multiple active outputs are unsupported; the global fish is disabled";
        return false;
    }
    if (!json_take(&reader, ']'))
        return false;
    json_space(&reader);
    if (reader.p != reader.end)
        return false;
    if (values[2] <= 0.0 || values[3] <= 0.0 || values[4] <= 0.0) {
        *reason = "j/monitors reports nonpositive output dimensions or scale";
        return false;
    }
    double width = values[2] / values[4], height = values[3] / values[4];
    if (!isfinite(width) || !isfinite(height) || width <= 0.0 || height <= 0.0 ||
        !isfinite(values[0] + width) || !isfinite(values[1] + height)) {
        *reason = "j/monitors reports unusable logical output bounds";
        return false;
    }
    *monitor = (struct monitor){ values[0], values[1], width, height, values[4] };
    return true;
}

static bool inside_monitor(double x, double y, const struct monitor *monitor)
{
    return x >= monitor->x && x < monitor->x + monitor->width &&
           y >= monitor->y && y < monitor->y + monitor->height;
}

static void invalidate(void)
{
    working.valid = false;
}

static void update_monitor(const struct monitor *monitor)
{
    if (monitor->x != working.monitor.x || monitor->y != working.monitor.y ||
        monitor->width != working.monitor.width || monitor->height != working.monitor.height ||
        monitor->scale != working.monitor.scale)
        invalidate();
    working.monitor = *monitor;
}

static void record_position(double x, double y)
{
    working.x = x;
    working.y = y;
    working.valid = true;
}

static void publish_state(void)
{
    pthread_mutex_lock(&state_mutex);
    published = working;
    pthread_mutex_unlock(&state_mutex);
}

static void sleep_until_next_poll(struct timespec *next)
{
    next->tv_nsec += POLL_NANOSECONDS;
    if (next->tv_nsec >= 1000000000L) {
        ++next->tv_sec;
        next->tv_nsec -= 1000000000L;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (next->tv_sec < now.tv_sec ||
        (next->tv_sec == now.tv_sec && next->tv_nsec <= now.tv_nsec)) {
        *next = now;
        next->tv_nsec += POLL_NANOSECONDS;
        if (next->tv_nsec >= 1000000000L) {
            ++next->tv_sec;
            next->tv_nsec -= 1000000000L;
        }
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL) == EINTR)
        ;
}

static void *tracker_worker(void *unused)
{
    (void)unused;
    bool monitor_known = false, outside_refreshed = false;
    double refresh_at = 0.0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        double now = monotonic_seconds();
        const char *reason;
        if (now >= refresh_at) {
            struct monitor monitor;
            monitor_known = refresh_monitor(&monitor, &reason);
            refresh_at = monotonic_seconds() + MONITOR_REFRESH_SECONDS;
            if (monitor_known) {
                update_monitor(&monitor);
            } else {
                invalidate();
                report_unavailable(reason);
            }
        }
        if (monitor_known) {
            double x, y;
            if (!ipc_request("j/cursorpos", cursor_reply, sizeof(cursor_reply))) {
                invalidate();
                report_unavailable(ipc_error);
            } else if (!parse_cursor(&x, &y)) {
                invalidate();
                report_unavailable("invalid j/cursorpos JSON or missing/duplicate finite x/y");
            } else {
                if (!inside_monitor(x, y, &working.monitor) && !outside_refreshed) {
                    struct monitor monitor;
                    monitor_known = refresh_monitor(&monitor, &reason);
                    refresh_at = monotonic_seconds() + MONITOR_REFRESH_SECONDS;
                    outside_refreshed = true;
                    if (monitor_known) {
                        update_monitor(&monitor);
                    } else {
                        invalidate();
                        report_unavailable(reason);
                    }
                }
                if (monitor_known && inside_monitor(x, y, &working.monitor)) {
                    record_position(x, y);
                    outside_refreshed = false;
                } else {
                    invalidate();
                }
            }
        }
        publish_state();
        sleep_until_next_poll(&next);
    }
    return NULL;
}

static void start_worker(void)
{
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error == 0) {
        size_t stack_size = 128U * 1024U;
        if (stack_size < (size_t)PTHREAD_STACK_MIN)
            stack_size = (size_t)PTHREAD_STACK_MIN;
        error = pthread_attr_setstacksize(&attributes, stack_size);
        if (error == 0)
            error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (error == 0) {
            pthread_t thread;
            error = pthread_create(&thread, &attributes, tracker_worker, NULL);
        }
        pthread_attr_destroy(&attributes);
    }
    if (error)
        fprintf(stderr, "[ascii-pointer] Cannot start global cursor tracker: %s; "
                "global fish interaction disabled.\n", strerror(error));
}

void pointer_tracker_start(void)
{
    pthread_once(&start_once, start_worker);
}

void pointer_tracker_snapshot(struct pointer_frame *frame)
{
    struct tracker_state state;
    pthread_mutex_lock(&state_mutex);
    state = published;
    pthread_mutex_unlock(&state_mutex);

    *frame = (struct pointer_frame){ .valid = state.valid };
    if (!state.valid)
        return;

    frame->x = (float)((state.x - state.monitor.x) / state.monitor.width);
    frame->y = (float)((state.y - state.monitor.y) / state.monitor.height);
    frame->aspect = (float)(state.monitor.width / state.monitor.height);
}

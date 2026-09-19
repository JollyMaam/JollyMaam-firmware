#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_LIMIT_MS 3000
#define QUIET_LIMIT_MS 250

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int open_one(const char *name, char *path, size_t size)
{
    int fd;
    snprintf(path, size, "%s", name);
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    return fd;
}

/* Development hosts expose devices in /dev/bwrap; try that namespace first. */
static int open_console(const char *requested_path, char *path, size_t size)
{
    static const char *const roots[] = { "/dev/bwrap", "/dev" };
    char candidate[128];
    unsigned int root, index;
    int fd;
    if (requested_path) return open_one(requested_path, path, size);
    for (root = 0; root < sizeof(roots) / sizeof(roots[0]); ++root)
        for (index = 0; index < 256U; ++index) {
            snprintf(candidate, sizeof(candidate), "%s/ttyACM%u", roots[root], index);
            fd = open_one(candidate, path, size);
            if (fd >= 0) return fd;
        }
    errno = ENODEV;
    return -1;
}

static int configure_console(int fd)
{
    struct termios attributes;
    if (tcgetattr(fd, &attributes) != 0) return -1;
    cfmakeraw(&attributes);
    cfsetispeed(&attributes, B115200);
    cfsetospeed(&attributes, B115200);
    attributes.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(fd, TCSANOW, &attributes) != 0) return -1;
#ifdef TIOCMBIS
    {
        int lines = TIOCM_DTR | TIOCM_RTS;
        if (ioctl(fd, TIOCMBIS, &lines) != 0 && errno != ENOTTY && errno != EINVAL)
            return -1;
        /* Some CDC bridges need a short interval after modem-line setup. */
        {
            struct timespec settle = { 0, 50000000L };
            nanosleep(&settle, NULL);
        }
    }
#endif
    return 0;
}

static int write_all_until(int fd, const char *buffer, size_t length,
                           long long deadline)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, buffer + offset, length - offset);
        if (count > 0) { offset += (size_t)count; continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = { fd, POLLOUT, 0 };
            int wait = (int)(deadline - monotonic_ms());
            if (wait <= 0 || poll(&descriptor, 1, wait) <= 0) return -1;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_command(const char *command, const char *requested_path)
{
    struct pollfd descriptor;
    char request[256], response[512], path[128];
    long long deadline, quiet_deadline;
    int fd, received = 0, result;
    fd = open_console(requested_path, path, sizeof(path));
    if (fd < 0) {
        fprintf(stderr, "cannot open Cyclone 2 console: %s\n", strerror(errno));
        return 1;
    }
    if (configure_console(fd) != 0) {
        fprintf(stderr, "%s: cannot configure raw serial port: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    result = snprintf(request, sizeof(request), "%s\r", command);
    if (result < 0 || (size_t)result >= sizeof(request)) {
        fprintf(stderr, "command is too long (maximum 254 bytes)\n");
        close(fd);
        return 2;
    }
    deadline = monotonic_ms() + RESPONSE_LIMIT_MS;
    if (write_all_until(fd, request, (size_t)result, deadline) != 0) {
        fprintf(stderr, "%s: command write failed: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    quiet_deadline = deadline;
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    while (monotonic_ms() < deadline) {
        long long target = received ? quiet_deadline : deadline;
        int wait = (int)(target - monotonic_ms());
        if (wait <= 0) break;
        result = poll(&descriptor, 1, wait);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(descriptor.revents & POLLIN)) continue;
        for (;;) {
            ssize_t length = read(fd, response, sizeof(response));
            if (length > 0) {
                fwrite(response, 1U, (size_t)length, stdout);
                fflush(stdout);
                received = 1;
                quiet_deadline = monotonic_ms() + QUIET_LIMIT_MS;
            } else if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                goto done;
            }
        }
    }
done:
    close(fd);
    if (!received) {
        fprintf(stderr, "%s: firmware did not answer before timeout\n", path);
        return 1;
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [-p /dev/ttyACM0] \"console command\"\n", program);
    fprintf(stderr, "       %s status                 # auto-discover CDC\n", program);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int argument = 1;
    size_t length = 0;
    char command[256];
    if (argument < argc && (!strcmp(argv[argument], "-h") || !strcmp(argv[argument], "--help"))) {
        usage(argv[0]);
        return 0;
    }
    if (argument < argc && (!strcmp(argv[argument], "-p") || !strcmp(argv[argument], "--path"))) {
        if (++argument >= argc) { usage(argv[0]); return 2; }
        path = argv[argument++];
    }
    if (argument >= argc) { usage(argv[0]); return 2; }
    if (argv[argument][0] == '-') { usage(argv[0]); return 2; }
    for (; argument < argc; ++argument) {
        size_t word = strlen(argv[argument]);
        if (length && length + 1U < sizeof(command)) command[length++] = ' ';
        if (length + word >= sizeof(command)) { usage(argv[0]); return 2; }
        memcpy(command + length, argv[argument], word);
        length += word;
    }
    command[length] = '\0';
    return send_command(command, path);
}

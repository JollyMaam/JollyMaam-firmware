#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int open_console(const char *requested_path, char *path, size_t size)
{
    unsigned int index;
    int fd;

    if (requested_path) {
        snprintf(path, size, "%s", requested_path);
        return open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    }

    for (index = 0; index < 256U; index++) {
        snprintf(path, size, "/dev/ttyACM%u", index);
        fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
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
    return tcsetattr(fd, TCSANOW, &attributes);
}

static int send_command(const char *command, const char *requested_path)
{
    struct pollfd descriptor;
    char request[96];
    char response[256];
    char path[64];
    ssize_t length;
    int received = 0;
    int fd;
    int result;

    fd = open_console(requested_path, path, sizeof(path));
    if (fd < 0) {
        fprintf(stderr, "cannot open Cyclone 2 console: %s\n", strerror(errno));
        return 1;
    }
    if (configure_console(fd) != 0) {
        fprintf(stderr, "%s: cannot configure serial port: %s\n", path,
                strerror(errno));
        close(fd);
        return 1;
    }

    result = snprintf(request, sizeof(request), "%s\r", command);
    if (result < 0 || (size_t)result >= sizeof(request) ||
        write(fd, request, (size_t)result) != result) {
        fprintf(stderr, "%s: command write failed: %s\n", path,
                strerror(errno));
        close(fd);
        return 1;
    }

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    for (;;) {
        /* Allow two seconds for the first response, then finish after the
         * console has been quiet for 250 ms. */
        result = poll(&descriptor, 1, received ? 250 : 2000);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        length = read(fd, response, sizeof(response));
        if (length > 0) {
            fwrite(response, 1U, (size_t)length, stdout);
            fflush(stdout);
            received = 1;
        } else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
    }
    close(fd);

    if (!received) {
        fprintf(stderr, "%s: firmware did not answer\n", path);
        return 1;
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s {help|status|uboot} [/dev/ttyACMN]\n", program);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 ||
        (strcmp(argv[1], "help") && strcmp(argv[1], "status") &&
         strcmp(argv[1], "uboot"))) {
        usage(argv[0]);
        return 2;
    }
    return send_command(argv[1], argc == 3 ? argv[2] : NULL);
}

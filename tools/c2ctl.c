#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cyclone2/usb_protocol.h"

#define JIELI_USB_VENDOR  0x4c4aU
#define JIELI_USB_PRODUCT 0x4155U

static const uint8_t protocol_magic[CYCLONE2_USB_MAGIC_SIZE] =
    CYCLONE2_USB_MAGIC_INITIALIZER;

static int open_controller(const char *requested_path, char *path, size_t size)
{
    unsigned int index;

    if (requested_path) {
        snprintf(path, size, "%s", requested_path);
        return open(path, O_RDWR | O_NONBLOCK);
    }

    for (index = 0; index < 256; index++) {
        struct hidraw_devinfo info;
        int fd;

        snprintf(path, size, "/dev/hidraw%u", index);
        fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0 && info.bustype == BUS_USB &&
            info.vendor == JIELI_USB_VENDOR &&
            info.product == JIELI_USB_PRODUCT) return fd;
        close(fd);
    }
    errno = ENODEV;
    return -1;
}

static int send_command(uint8_t command, const char *requested_path)
{
    /* hidraw requires a leading zero report ID for an unnumbered output
     * report. The firmware receives only the following 64-byte payload. */
    uint8_t output[CYCLONE2_USB_PACKET_SIZE + 1U] = {0};
    uint8_t input[CYCLONE2_USB_PACKET_SIZE + 1U];
    struct pollfd poll_fd;
    char path[64];
    size_t offset = 0;
    ssize_t length;
    int fd;

    fd = open_controller(requested_path, path, sizeof(path));
    if (fd < 0) {
        fprintf(stderr, "cannot open Cyclone 2 configuration HID: %s\n",
                strerror(errno));
        return 1;
    }
    memcpy(output + 1U, protocol_magic, sizeof(protocol_magic));
    output[1U + CYCLONE2_USB_COMMAND_OFFSET] = command;
    output[1U + CYCLONE2_USB_INVERSE_OFFSET] = (uint8_t)~command;

    length = write(fd, output, sizeof(output));
    if (length != (ssize_t)sizeof(output)) {
        fprintf(stderr, "%s: command write failed: %s\n", path,
                length < 0 ? strerror(errno) : "short write");
        close(fd);
        return 1;
    }

    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    if (poll(&poll_fd, 1, 1000) <= 0) {
        fprintf(stderr, "%s: firmware did not acknowledge command\n", path);
        close(fd);
        return 1;
    }
    length = read(fd, input, sizeof(input));
    close(fd);
    /* Linux normally omits report ID zero on reads, but accept it if a HID
     * transport supplies the same leading byte required on writes. */
    if (length > (ssize_t)CYCLONE2_USB_MAGIC_SIZE && input[0] == 0U &&
        !memcmp(input + 1U, protocol_magic, sizeof(protocol_magic))) offset = 1U;
    if (length < (ssize_t)(offset + CYCLONE2_USB_INVERSE_OFFSET + 1U) ||
        memcmp(input + offset, protocol_magic, sizeof(protocol_magic)) ||
        input[offset + CYCLONE2_USB_COMMAND_OFFSET] !=
            (command | CYCLONE2_USB_RESPONSE_BIT) ||
        input[offset + CYCLONE2_USB_INVERSE_OFFSET] !=
            (uint8_t)~input[offset + CYCLONE2_USB_COMMAND_OFFSET]) {
        fprintf(stderr, "%s: invalid firmware response\n", path);
        return 1;
    }

    if (command == CYCLONE2_USB_COMMAND_STATUS) {
        uint32_t magic;
        size_t data = offset + CYCLONE2_USB_DATA_OFFSET;
        size_t index;
        if (length < (ssize_t)(data + 24U)) {
            fprintf(stderr, "%s: short status response\n", path);
            return 1;
        }
        memcpy(&magic, input + data, sizeof(magic));
        printf("%s: reset mailbox magic=%08x profile=%u pairing=%u check=%02x%02x nvram=",
               path, magic, input[data + 4U], input[data + 5U],
               input[data + 7U], input[data + 6U]);
        for (index = 0; index < 16U; index++) printf("%02x", input[data + 8U + index]);
        putchar('\n');
    } else {
        size_t data = offset + CYCLONE2_USB_DATA_OFFSET;
        size_t index;
        printf("%s: UBOOT1.00 request accepted; armed nvram=", path);
        for (index = 0; index < 16U; index++) printf("%02x", input[data + index]);
        putchar('\n');
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s {uboot|status} [/dev/hidrawN]\n", program);
}

int main(int argc, char **argv)
{
    uint8_t command;
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "uboot")) command = CYCLONE2_USB_COMMAND_UBOOT;
    else if (!strcmp(argv[1], "status")) command = CYCLONE2_USB_COMMAND_STATUS;
    else {
        usage(argv[0]);
        return 2;
    }
    return send_command(command, argc == 3 ? argv[2] : NULL);
}

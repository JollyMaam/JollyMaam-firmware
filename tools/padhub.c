/* padhub: a dongle in hub mode (link.h) becomes controllers on this Linux PC.
 *
 *   make padhub
 *   build/host/padhub
 *
 * It finds the dongle (our firmware's tool device, HID) and turns on
 * its hub mode; whenever a pad is linked the dongle is then a vendor HID
 * device that carries its pads' link packets as they are.  Closed (Ctrl-C,
 * SIGTERM), padhub turns hub mode off again: the dongle is the tool device
 * until its pad next links, then itself (the pad's type on USB).  For each slot a child
 * process runs the firmware's own profile code (src/profiles/, the slot's
 * type from the pad's config) and puts what it builds on a uhid device, as
 * the dongle would on its own USB port: hid-playstation, hid-nintendo and
 * SDL take it for the real thing.  The host's writes go back to the
 * profile; rumble and lights go to the pad.  A child per slot: the profiles
 * keep their state in statics, one controller each.
 *
 * Needs /dev/uhid and the dongle's hidraw nodes writable (root or udev
 * rules).  With several dongles: the first it finds. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/uhid.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "pad.h"
#include "link.h"

#define GRACE_MS        3000     /* as the dongle: a pad gone this long, its controller goes */
#define REPORT_IDLE_MS  8        /* unchanged state is sent this often */
#define FIND_MS         1000

static int hidraw = -1;

/* ---- what the profiles need from a platform ------------------------------ */

const char board_name[] = "Cyclone 2";
static u8 uid[16];
static int uhid = -1;
static const struct usb_identity *usb;
static u8 slot;

static u32 now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u32)((u64)t.tv_sec * 1000000u + (u64)t.tv_nsec / 1000u);
}

u32 plat_us(void) { return now_us(); }
u32 plat_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u32)((u64)t.tv_sec * 1000u + (u64)t.tv_nsec / 1000000u);
}
void plat_lock(void) {}
void plat_unlock(void) {}
const u8 *plat_unique_id(void) { return uid; }

/* The profile's log lines (utils.c's ring), on stderr. */
static void flush_log(void)
{
    static u32 seq = 1;
    char text[2048];
    u32 n;

    while ((n = util_log_read(&seq, text, sizeof(text) - 1)) > 0) {
        const char *line = text, *end;

        text[n] = 0;
        while ((end = strchr(line, '\n'))) {
            fprintf(stderr, "padhub: slot %u: %.*s\n", slot, (int)(end - line), line);
            line = end + 1;
            seq++;
        }
    }
}

static int uhid_send(struct uhid_event *ev)
{
    return write(uhid, ev, sizeof(*ev)) == (ssize_t)sizeof(*ev);
}

static int input(u8 id, const u8 *data, u16 len)
{
    struct uhid_event ev;
    u8 at = usb->report_ids ? 1 : 0;

    if (uhid < 0 || len + at > UHID_DATA_MAX) return 0;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;
    if (at) ev.u.input2.data[0] = id;
    memcpy(ev.u.input2.data + at, data, len);
    ev.u.input2.size = (u16)(len + at);
    return uhid_send(&ev);
}

/* A profile's message to the host: an input report too (no bulk here). */
int host_send(u16 id, const u8 *data, u16 len)
{
    if (id & HOST_BULK) return 0;
    return input((u8)id, data, len);
}

/* ---- one slot (the child) --------------------------------------------- */

static struct {
    const struct profile *prof;
    struct set_slot set;
    struct profile_session sess;
    struct profile_host host;
    struct bt_bond bond;
    u8 type, variant, begin_imu, started;
    struct pad pad;
    s32 gyro_bias[3];
    u8 gyro_bias_known, fresh, down;
    u32 down_ms, report_ms, output_ms;
    u8 output[LINK_OUTPUT_SIZE];
} c;

static void destroy(void)
{
    struct uhid_event ev;

    if (uhid < 0) return;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    uhid_send(&ev);
    close(uhid);
    uhid = -1;
    if (c.prof && c.prof->end) c.prof->end();
    fprintf(stderr, "padhub: slot %u: %s gone\n", slot, c.prof ? c.prof->name : "?");
    c.prof = NULL;
    c.started = 0;
    flush_log();
}

static void create(const struct link_config *k)
{
    const struct profile *p = profile_for(k->type);
    struct uhid_event ev;
    u32 i;

    for (i = 0; i < 3; i++) c.gyro_bias[i] = k->gyro_bias[i];
    c.gyro_bias_known = k->gyro_bias_known;
    if (c.prof && c.type == k->type && c.variant == k->variant &&
        c.set.stick_speed == k->stick_speed && c.set.gyro_speed == k->gyro_speed &&
        c.set.gyro_accel == k->gyro_accel && !memcmp(c.sess.address, k->address, 6)) return;
    destroy();
    if (!p || !p->usb) {
        fprintf(stderr, "padhub: slot %u: type %u has no USB device\n", slot, k->type);
        return;
    }
    if (p->usb->kind != USB_HID || p->usb->hid_descriptor_len > HID_MAX_DESCRIPTOR_SIZE) {
        /* XInput is a vendor interface xpad wants on USB; Switch 2's handshake is bulk. */
        fprintf(stderr, "padhub: slot %u: %s is not HID only, uhid cannot be it\n", slot, p->name);
        return;
    }
    uhid = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (uhid < 0 && errno == ENOENT)
        uhid = open("/dev/bwrap/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (uhid < 0) {
        fprintf(stderr, "padhub: /dev/uhid: %s\n", strerror(errno));
        return;
    }
    usb = p->usb;
    memset(uid, 0, sizeof(uid));
    memcpy(uid, k->address, 6);
    memset(&c.set, 0, sizeof(c.set));
    c.set.type = k->type;
    c.set.variant = k->variant;
    c.set.stick_speed = k->stick_speed;
    c.set.gyro_speed = k->gyro_speed;
    c.set.gyro_accel = k->gyro_accel;
    memset(&c.sess, 0, sizeof(c.sess));
    memcpy(c.sess.address, k->address, 6);
    c.sess.settings = &c.set;
    c.sess.usb = 1;
    memset(&c.host, 0, sizeof(c.host));
    memset(&c.pad, 0, sizeof(c.pad));
    c.prof = p;
    c.type = k->type;
    c.variant = k->variant;
    c.begin_imu = p->begin(&c.sess);
    c.fresh = 1;

    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "%s%s%s",
             usb->manufacturer ? usb->manufacturer : "", usb->manufacturer ? " " : "",
             usb->product ? usb->product : p->name);
    snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys), "padhub/slot%u", slot);
    snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq), "%02x:%02x:%02x:%02x:%02x:%02x",
             k->address[5], k->address[4], k->address[3], k->address[2], k->address[1], k->address[0]);
    ev.u.create2.rd_size = usb->hid_descriptor_len;
    ev.u.create2.bus = BUS_USB;
    ev.u.create2.vendor = usb->vendor_id;
    ev.u.create2.product = usb->product_id;
    ev.u.create2.version = usb->bcd_device;
    memcpy(ev.u.create2.rd_data, usb->hid_descriptor, usb->hid_descriptor_len);
    if (!uhid_send(&ev)) {
        fprintf(stderr, "padhub: slot %u: UHID_CREATE2: %s\n", slot, strerror(errno));
        close(uhid);
        uhid = -1;
        if (c.prof->end) c.prof->end();
        c.prof = NULL;
        return;
    }
    fprintf(stderr, "padhub: slot %u: %s (%04x:%04x)\n", slot, p->name, usb->vendor_id, usb->product_id);
    flush_log();
}

/* The newest state replaces the old; motion samples add up until a report
 * takes them (IMU_PER_REPORT at most, the newest), as on the dongle. */
static void take_input(const u8 *d, u16 n)
{
    struct pad p;
    struct imu_sample all[2 * IMU_PER_REPORT];
    u8 seq, flags;
    u32 i, k = 0;

    if (!link_unpack_input(d, n, &p, &seq, &flags)) return;
    for (i = 0; i < c.pad.imu_count; i++) all[k++] = c.pad.imu[i];
    for (i = 0; i < p.imu_count; i++) all[k++] = p.imu[i];
    p.imu_count = (u8)(k > IMU_PER_REPORT ? IMU_PER_REPORT : k);
    memcpy(p.imu, all + (k - p.imu_count), p.imu_count * sizeof(all[0]));
    c.pad = p;
    c.fresh = 1;
}

static void report(u32 now)
{
    u8 buf[UHID_DATA_MAX], id = 0;
    int n;

    if (!c.prof || !c.started || (!c.fresh && now - c.report_ms < REPORT_IDLE_MS)) return;
    memcpy(c.pad.gyro_bias, c.gyro_bias, sizeof(c.gyro_bias));
    c.pad.gyro_bias_known = c.gyro_bias_known;
    n = c.prof->report(&c.pad, now, buf, &id);
    c.pad.imu_count = 0;
    c.fresh = 0;
    c.report_ms = now;
    if (n > 0) input(id, buf, (u16)n);
}

/* Rumble and lights to the pad: on change and every LINK_OUTPUT_MS. */
static void output(u32 now)
{
    u8 out[LINK_OUTPUT_SIZE], r[1 + HUB_REPORT_SIZE];

    if (!c.prof) return;
    if (c.prof->host) c.prof->host(now, &c.host);
    link_pack_output(&c.host, c.host.imu ? c.host.imu : c.begin_imu, c.started, out);
    if (!memcmp(out, c.output, sizeof(out)) && now - c.output_ms < LINK_OUTPUT_MS) return;
    memcpy(c.output, out, sizeof(out));
    c.output_ms = now;
    memset(r, 0, sizeof(r));
    r[0] = HUB_REPORT_OUT;
    r[1] = slot;
    r[2] = HUB_OUTPUT;
    memcpy(r + 3, out, sizeof(out));
    if (write(hidraw, r, sizeof(r)) != (ssize_t)sizeof(r) && errno != EAGAIN)
        fprintf(stderr, "padhub: slot %u: hidraw write: %s\n", slot, strerror(errno));
}

/* GET_REPORT carries no requested length. Derive the payload length from
 * our descriptor: profiles fill exactly the size their caller supplies. */
static u16 feature_size(u8 report_id)
{
    struct globals { u32 size, count, id; } g = { 0 }, stack[8];
    u32 at = 0, bits = 0, depth = 0;

    while (at < usb->hid_descriptor_len) {
        u8 item = usb->hid_descriptor[at++];
        u32 n = item & 3, v = 0, i;

        if (item == 0xfe) return 0; /* none of our descriptors uses long items */
        if (n == 3) n = 4;
        if (n > usb->hid_descriptor_len - at) return 0;
        for (i = 0; i < n; i++) v |= (u32)usb->hid_descriptor[at++] << (8 * i);
        switch (item & 0xfc) {
        case 0x74: g.size = v; break;
        case 0x94: g.count = v; break;
        case 0x84: g.id = v; break;
        case 0xa4:
            if (depth == 8) return 0;
            stack[depth++] = g;
            break;
        case 0xb4:
            if (!depth) return 0;
            g = stack[--depth];
            break;
        case 0xb0:
            if (g.id != report_id) break;
            if (g.size > 32 || g.count > UHID_DATA_MAX ||
                bits + g.size * g.count > 8 * (UHID_DATA_MAX - 1)) return 0;
            bits += g.size * g.count;
            break;
        }
    }
    return (u16)((bits + 7) / 8);
}

static void from_kernel(u32 now)
{
    struct uhid_event ev, re;
    struct bt_write w;
    u8 at = usb->report_ids ? 1 : 0;
    const u8 *d;
    u32 n;

    memset(&ev, 0, sizeof(ev));
    if (read(uhid, &ev, sizeof(ev)) <= 0) return;
    memset(&w, 0, sizeof(w));
    switch (ev.type) {
    case UHID_START:
        c.started = 1;
        c.fresh = 1;
        break;
    case UHID_STOP:
        c.started = 0;
        break;
    case UHID_OUTPUT:
        if (!c.prof->write || ev.u.output.rtype != UHID_OUTPUT_REPORT) break;
        d = ev.u.output.data;
        n = ev.u.output.size;
        if (at && n) { w.id = d[0]; d++; n--; }
        if (n > BT_WRITE_MAX) n = BT_WRITE_MAX;
        w.kind = BT_WRITE_OUTPUT;
        w.len = (u16)n;
        memcpy(w.data, d, n);
        c.prof->write(&w, now, &c.bond);
        break;
    case UHID_SET_REPORT:
        d = ev.u.set_report.data;
        n = ev.u.set_report.size;
        if (at && n) { d++; n--; }
        if (n > BT_WRITE_MAX) n = BT_WRITE_MAX;
        w.kind = ev.u.set_report.rtype == UHID_FEATURE_REPORT ? BT_WRITE_FEATURE : BT_WRITE_OUTPUT;
        w.id = ev.u.set_report.rnum;
        w.len = (u16)n;
        memcpy(w.data, d, n);
        if (c.prof->write) c.prof->write(&w, now, &c.bond);
        memset(&re, 0, sizeof(re));
        re.type = UHID_SET_REPORT_REPLY;
        re.u.set_report_reply.id = ev.u.set_report.id;
        uhid_send(&re);
        break;
    case UHID_GET_REPORT: {
        int got = -1;
        u16 size = feature_size(ev.u.get_report.rnum);

        memset(&re, 0, sizeof(re));
        re.type = UHID_GET_REPORT_REPLY;
        re.u.get_report_reply.id = ev.u.get_report.id;
        if (size && c.prof->read && ev.u.get_report.rtype == UHID_FEATURE_REPORT)
            got = c.prof->read(BT_WRITE_FEATURE, ev.u.get_report.rnum, 0, re.u.get_report_reply.data + at,
                               size);
        if (got > 0) {
            if (at) re.u.get_report_reply.data[0] = ev.u.get_report.rnum;
            re.u.get_report_reply.size = (u16)(got + at);
        } else {
            re.u.get_report_reply.err = EIO;
        }
        uhid_send(&re);
        break;
    }
    }
    flush_log();
}

/* Packets for this slot arrive on sock from the parent; EOF: the dongle is gone. */
static void slot_main(int sock)
{
    u8 p[HUB_REPORT_SIZE + 1];
    struct link_config k;

    signal(SIGINT, SIG_IGN);     /* the parent closes our socket instead */
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        struct pollfd f[2] = { { sock, POLLIN, 0 }, { uhid, POLLIN, 0 } };
        u32 now = plat_ms(), wait = LINK_OUTPUT_MS;
        ssize_t n;

        if (c.prof && c.started) wait = REPORT_IDLE_MS;
        if (poll(f, uhid >= 0 ? 2 : 1, (int)wait) < 0 && errno != EINTR) break;
        now = plat_ms();
        if (f[0].revents & (POLLERR | POLLNVAL)) break;
        if (f[0].revents & (POLLIN | POLLHUP)) {
            unsigned drained;

            /* Fold queued state into one report, including its motion samples.
             * Bound the batch so kernel requests and output still get serviced. */
            for (drained = 0; drained < 64; drained++) {
                n = recv(sock, p, sizeof(p), MSG_DONTWAIT);
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (n <= 0) goto done;
                if (n >= 2 && p[1] == HUB_CONFIG && link_unpack_config(p + 2, (u16)(n - 2), &k)) {
                    c.down = 0;
                    create(&k);
                } else if (n >= 2 && p[1] == HUB_INPUT && c.prof) {
                    c.down = 0;
                    take_input(p + 2, (u16)(n - 2));
                } else if (n >= 2 && p[1] == HUB_DOWN && !c.down) {
                    c.down = 1;
                    c.down_ms = now;
                    memset(&c.pad, 0, sizeof(c.pad));     /* at rest until it is back or gone */
                    c.fresh = 1;
                    fprintf(stderr, "padhub: slot %u: the pad is away\n", slot);
                }
            }
        }
        /* Nonblocking read also handles a config replacing the uhid device
         * between poll and here, including reuse of the same fd number. */
        if (uhid >= 0 && uhid == f[1].fd && (f[1].revents & POLLIN)) from_kernel(now);
        if (c.down && now - c.down_ms >= GRACE_MS) {
            destroy();
            c.down = 0;          /* keep this worker ready for a reconnect */
        }
        report(now);
        output(now);
    }
done:
    destroy();
    close(sock);
    close(hidraw);
    exit(0);
}

/* ---- the dongle (the parent) -------------------------------------------- */

static struct { pid_t pid; int sock; } child[HUB_SLOTS];

static void stop_children(void)
{
    u32 i;

    for (i = 0; i < HUB_SLOTS; i++)
        if (child[i].pid > 0) {
            close(child[i].sock);
            child[i].pid = 0;
        }
}

static void reap(void)
{
    pid_t pid;
    u32 i;

    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
        for (i = 0; i < HUB_SLOTS; i++)
            if (child[i].pid == pid) {
                close(child[i].sock);
                child[i].pid = 0;
            }
}

static int spawn(u8 s)
{
    int sv[2];
    pid_t pid;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) return 0;
    pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return 0;
    }
    if (!pid) {
        u32 i;

        for (i = 0; i < HUB_SLOTS; i++)
            if (child[i].pid > 0) close(child[i].sock);
        close(sv[0]);
        slot = s;
        slot_main(sv[1]);
    }
    close(sv[1]);
    child[s].pid = pid;
    child[s].sock = sv[0];
    return 1;
}

/* ---- the tool device: usb.c's commands as HID feature reports ------------ */

#define TOOL_REPORT 512          /* the Jieli tool device's (platform_jieli_usb.c) */

/* A command and up to len bytes of its answer: the report (CDB, u16
 * length, flags, tag), then reads of it until the last piece.  -1: none. */
static int tool_in(const char *path, const u8 cdb[16], u8 *data, u32 len)
{
    static u8 tag;
    u8 buf[TOOL_REPORT + 1];
    u32 got = 0, busy = 0, n;
    int fd = open(path, O_RDWR | O_CLOEXEC), ok = 0;

    if (fd < 0) return -1;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + 1, cdb, 16);
    buf[17] = (u8)len;
    buf[18] = (u8)(len >> 8);
    buf[20] = ++tag;
    if (ioctl(fd, HIDIOCSFEATURE(sizeof(buf)), buf) < 0) goto done;
    while (busy < 1000) {
        buf[0] = 0;
        if (ioctl(fd, HIDIOCGFEATURE(sizeof(buf)), buf) < 0 || buf[1] != tag) break;
        if (buf[2] == 1) {               /* busy */
            busy++;
            usleep(1000);
            continue;
        }
        if (buf[2] != 2 && buf[2] != 3) break;
        n = (u32)(buf[3] | buf[4] << 8);
        if (n > len - got) n = len - got;
        memcpy(data + got, buf + 5, n);
        got += n;
        if (buf[2] == 3) {
            ok = 1;
            break;
        }
    }
done:
    close(fd);
    return ok ? (int)got : -1;
}

/* Whether hidraw node name is a tool device: its usage page. */
static int is_tool(const char *name)
{
    char sys[300];
    u8 d[3];
    FILE *f;
    int ok;

    snprintf(sys, sizeof(sys), "/sys/class/hidraw/%s/device/report_descriptor", name);
    if (!(f = fopen(sys, "rb"))) return 0;
    ok = fread(d, 1, 3, f) == 3 && d[0] == 0x06 && d[1] == 0xc2 && d[2] == 0xff;
    fclose(f);
    return ok;
}

/* A dongle running this firmware at path: our IDENTIFY says so. */
static int is_dongle(const char *path)
{
    u8 cdb[16] = { 0xf5, 0x00 }, d[88];

    return tool_in(path, cdb, d, sizeof(d)) >= 56 && !memcmp(d, "C2PD", 4) && d[6] == 1;
}

/* Hub mode on (1) or off (0) on the first dongle that is the tool device:
 * 1 done, 0 none there. */
static int tool_hub(u8 on)
{
    static const char *const dirs[] = { "/dev", "/dev/bwrap" };
    char path[300];
    u8 cdb[16] = { 0xf5, 0x32, 0xff }, mode;
    u32 i;

    for (i = 0; i < 2; i++) {
        DIR *dir = opendir(dirs[i]);
        struct dirent *e;

        if (!dir) continue;
        while ((e = readdir(dir))) {
            if (strncmp(e->d_name, "hidraw", 6) || !is_tool(e->d_name)) continue;
            snprintf(path, sizeof(path), "%s/%s", dirs[i], e->d_name);
            if (access(path, R_OK | W_OK)) continue;
            if (!is_dongle(path) || tool_in(path, cdb, &mode, 1) != 1) continue;
            closedir(dir);
            if (mode == on) return 1;        /* as it was: quiet */
            cdb[2] = on;
            if (tool_in(path, cdb, &mode, 1) != 1) return 0;
            fprintf(stderr, "padhub: dongle at %s: hub mode %s\n", path, mode ? "on" : "off");
            return 1;
        }
        closedir(dir);
    }
    return 0;
}

/* The hidraw node whose device is the hub's VID:PID with the hub's usage
 * page (the tool device shares the ID), else NULL. */
static const char *find(void)
{
    static char path[300];
    char want[40], line[256], name[300];
    DIR *dir = opendir("/sys/class/hidraw");
    struct dirent *e;

    if (!dir) return NULL;
    snprintf(want, sizeof(want), "HID_ID=0003:%08X:%08X", HUB_VENDOR_ID, HUB_PRODUCT_ID);
    while ((e = readdir(dir))) {
        FILE *f;
        int match = 0;

        if (strncmp(e->d_name, "hidraw", 6)) continue;
        snprintf(name, sizeof(name), "/sys/class/hidraw/%s/device/uevent", e->d_name);
        if (!(f = fopen(name, "r"))) continue;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, want, strlen(want))) match = 1;
        fclose(f);
        if (!match) continue;
        snprintf(name, sizeof(name), "/sys/class/hidraw/%s/device/report_descriptor", e->d_name);
        if (!(f = fopen(name, "rb"))) continue;
        match = fread(line, 1, 3, f) == 3 && (u8)line[0] == 0x06 &&
                ((u8)line[1] | (u8)line[2] << 8) == HUB_USAGE_PAGE;
        fclose(f);
        if (!match) continue;
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        if (access(path, F_OK)) snprintf(path, sizeof(path), "/dev/bwrap/%s", e->d_name);
        closedir(dir);
        return path;
    }
    closedir(dir);
    return NULL;
}

static volatile sig_atomic_t quit;
static void on_signal(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv)
{
    u8 p[1 + HUB_REPORT_SIZE + 1];
    int waiting = 0, told = 0;

    if (argc > 1) {
        fprintf(stderr, "usage: %s\n", argv[0]);
        return 2;
    }
    {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;   /* no SA_RESTART: the read ends */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
    signal(SIGPIPE, SIG_IGN);
    while (!quit) {
        const char *path = find();

        if (!path || (hidraw = open(path, O_RDWR | O_CLOEXEC)) < 0) {
            if (path && !waiting) fprintf(stderr, "padhub: %s: %s\n", path, strerror(errno));
            /* The tool device: (again) in hub mode, after a replug too. */
            if (!path && tool_hub(1)) told = 1;
            if (!waiting) fprintf(stderr, told ? "padhub: waiting for a pad to link\n" :
                                          "padhub: waiting for the dongle\n");
            waiting = 1;
            usleep(FIND_MS * 1000);
            continue;
        }
        waiting = 0;
        fprintf(stderr, "padhub: hub at %s\n", path);
        while (!quit) {
            struct pollfd f = { hidraw, POLLIN, 0 };
            ssize_t n;
            u8 s;
            int ready;

            reap();
            ready = poll(&f, 1, FIND_MS);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || (f.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            if (!ready) continue;
            n = read(hidraw, p, sizeof(p));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            if (n < 3 || p[0] != HUB_REPORT_IN) continue;
            s = p[1];
            if (s >= HUB_SLOTS) continue;
            if (!child[s].pid && p[2] == HUB_CONFIG && !spawn(s)) continue;
            if (child[s].pid && send(child[s].sock, p + 1, (size_t)n - 1, MSG_DONTWAIT) < 0 && errno != EAGAIN)
                fprintf(stderr, "padhub: slot %u: %s\n", s, strerror(errno));
        }
        stop_children();
        if (quit) break;
        fprintf(stderr, "padhub: the hub is gone\n");
        close(hidraw);
        hidraw = -1;
    }
    stop_children();
    while (wait(NULL) > 0) {}
    /* Hub mode off: "tool" to the hub, else to the tool device. */
    if (hidraw >= 0) {
        u8 tool[5] = { USB_TOOL_REPORT, 't', 'o', 'o', 'l' };

        if (ioctl(hidraw, HIDIOCSFEATURE(sizeof(tool)), tool) < 0 && errno != EPROTO)
            fprintf(stderr, "padhub: hub mode off: %s\n", strerror(errno));
        else
            fprintf(stderr, "padhub: hub mode off\n");
        close(hidraw);
    } else {
        tool_hub(0);
    }
    return 0;
}

/* Host protocol and worker lifecycle checks without /dev/uhid or a dongle. */
#define main padhub_main
#include "padhub.c"
#undef main
#include <assert.h>

static void kernel_event(int peer, struct uhid_event *ev, struct uhid_event *reply)
{
    assert(write(peer, ev, sizeof(*ev)) == sizeof(*ev));
    from_kernel(1000);
    assert(read(peer, reply, sizeof(*reply)) == sizeof(*reply));
}

int main(void)
{
    int sv[2], status;
    struct uhid_event ev = { 0 }, reply;
    struct profile_session session = { 0 };
    struct pad p = { 0 };
    u8 packet[LINK_INPUT_SIZE];
    u32 before;
    pid_t pid;

    /* Milliseconds must not inherit the 32-bit microsecond clock's wrap. */
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    before = (u32)((u64)t.tv_sec * 1000 + t.tv_nsec / 1000000);
    assert(plat_ms() - before < 100);

    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    uhid = sv[0];
    c.prof = &profile_ds5edge;
    usb = c.prof->usb;
    session.usb = 1;
    session.address[0] = 42;
    c.prof->begin(&session);
    assert(feature_size(0x05) == 40);
    assert(feature_size(0x09) == 19);
    assert(feature_size(0xff) == 0);

    ev.type = UHID_GET_REPORT;
    ev.u.get_report.id = 123;
    ev.u.get_report.rtype = UHID_FEATURE_REPORT;
    ev.u.get_report.rnum = 0x05;
    kernel_event(sv[1], &ev, &reply);
    assert(reply.type == UHID_GET_REPORT_REPLY);
    assert(reply.u.get_report_reply.id == 123 && !reply.u.get_report_reply.err);
    assert(reply.u.get_report_reply.size == 41 && reply.u.get_report_reply.data[0] == 5);
    assert((reply.u.get_report_reply.data[7] | (reply.u.get_report_reply.data[8] << 8)) == 8000);

    ev.u.get_report.rnum = 0xff;
    kernel_event(sv[1], &ev, &reply);
    assert(reply.u.get_report_reply.err == EIO && !reply.u.get_report_reply.size);

    /* Kernel output runs through the real profile and becomes slot-tagged
     * compact rumble output to the dongle. */
    int radio[2];
    u8 out[1 + HUB_REPORT_SIZE];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, radio) == 0);
    hidraw = radio[0];
    slot = HUB_SLOTS - 1;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_OUTPUT;
    ev.u.output.rtype = UHID_OUTPUT_REPORT;
    ev.u.output.size = 48;
    ev.u.output.data[0] = 2;
    ev.u.output.data[1] = 1;
    ev.u.output.data[3] = 60;
    ev.u.output.data[4] = 80;
    assert(write(sv[1], &ev, sizeof(ev)) == sizeof(ev));
    from_kernel(1000);
    c.started = 1;
    output(1000);
    assert(read(radio[1], out, sizeof(out)) == sizeof(out));
    assert(out[0] == HUB_REPORT_OUT && out[1] == slot && out[2] == HUB_OUTPUT);
    assert(out[3] == 80 && out[4] == 60 && (out[5] & LINK_USB_READY));
    close(radio[0]);
    close(radio[1]);
    hidraw = -1;

    /* Input folding preserves the newest buttons and gathers motion once. */
    p.buttons = PAD_SOUTH;
    p.imu_count = 1;
    p.imu[0].us = 100;
    link_pack_input(&p, 1, 0, packet);
    take_input(packet, sizeof(packet));
    p.buttons = PAD_EAST;
    p.imu[0].us = 200;
    link_pack_input(&p, 2, 0, packet);
    take_input(packet, sizeof(packet));
    assert(c.pad.buttons == PAD_EAST && c.pad.imu_count == 2);
    assert(c.pad.imu[0].us == 100 && c.pad.imu[1].us == 200);
    c.started = 1;
    report(1000);
    assert(read(sv[1], &reply, sizeof(reply)) == sizeof(reply));
    assert(reply.type == UHID_INPUT2 && reply.u.input2.data[0] == 1);
    assert(!c.pad.imu_count && !c.fresh);
    destroy();
    assert(read(sv[1], &reply, sizeof(reply)) == sizeof(reply));
    assert(reply.type == UHID_DESTROY);
    close(sv[1]);

    /* An expired slot keeps its worker; EOF reliably shuts it down. */
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        close(sv[0]);
        c.down = 1;
        c.down_ms = plat_ms() - GRACE_MS;
        slot_main(sv[1]);
    }
    close(sv[1]);
    usleep(200000);
    assert(waitpid(pid, &status, WNOHANG) == 0);
    close(sv[0]);
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("padhub: ok");
    return 0;
}

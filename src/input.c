#include "pad.h"

#define MOVED_STICK   2048
#define MOVED_TRIGGER 4096

/* Buttons and d-pad as sampled; sticks and triggers are filled in by
 * cal_apply() (input_calibration.c). */
void in_map(const struct in_raw *raw, struct pad *pad)
{
    pad->buttons = raw->buttons;
    pad->hat = raw->hat;
}

static int far(s32 a, s32 b, s32 limit)
{
    return a - b >= limit || b - a >= limit;
}

int in_moved(const struct pad *now, const struct pad *ref)
{
    return now->buttons != ref->buttons || now->hat != ref->hat ||
           far(now->lx, ref->lx, MOVED_STICK) || far(now->ly, ref->ly, MOVED_STICK) ||
           far(now->rx, ref->rx, MOVED_STICK) || far(now->ry, ref->ry, MOVED_STICK) ||
           far(now->l2, ref->l2, MOVED_TRIGGER) || far(now->r2, ref->r2, MOVED_TRIGGER);
}

#include "pad.h"

static const struct set_prefs prefs = {
    .idle_colour = { 5, 5, 5 },
    .battery_colour = { 5, 2, 0 },
};

static const struct set_slot slots[SLOTS] = {
    /* A */     { PROFILE_SWITCH,  1, GRIPS_LIGHTBAR, { 0, 0, 5 } },
    /* B */     { PROFILE_SWITCH,  1, GRIPS_LIGHTBAR, { 5, 0, 0 } },
    /* X */     { PROFILE_GENERIC, 1, GRIPS_LIGHTBAR, { 3, 0, 5 } },
    /* Y */     { PROFILE_SWITCH2, 1, GRIPS_LIGHTBAR, { 0, 5, 0 } },
    /* up */    { PROFILE_MOUSE,   1, GRIPS_LIGHTBAR, { 0, 4, 4 } },
    /* down */  { PROFILE_MOUSE,   1, GRIPS_LIGHTBAR, { 4, 4, 0 } },
    /* left */  { PROFILE_MOUSE,   1, GRIPS_LIGHTBAR, { 4, 0, 3 } },
    /* right */ { PROFILE_MOUSE,   1, GRIPS_LIGHTBAR, { 2, 4, 0 } },
};

const struct set_slot *set_slot(u8 slot)
{
    return &slots[slot % SLOTS];
}

const struct set_prefs *set_prefs(void)
{
    return &prefs;
}

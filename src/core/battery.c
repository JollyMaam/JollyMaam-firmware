#include "pad.h"

/* Update measured voltage and a board's linear voltage-based estimate.
 * Zero voltage is unreadable; empty/full both zero omit the estimate.
 * The percentage is an estimate of charge, not a charger-completion flag. */
void battery_voltage(struct battery_status *b, u16 mv, u16 empty_mv, u16 full_mv)
{
    b->mv = mv;
    b->percent = 0;
    b->flags &= (u8)~(JM_BATTERY_MV_VALID | JM_BATTERY_PERCENT_VALID);
    if (!mv) return;
    b->flags |= JM_BATTERY_MV_VALID;
    if (full_mv <= empty_mv) return;
    b->percent = mv <= empty_mv ? 0 : mv >= full_mv ? 100 :
        (u8)((u32)(mv - empty_mv) * 100 / (full_mv - empty_mv));
    b->flags |= JM_BATTERY_PERCENT_VALID;
}

/* Apply board-owned warning thresholds, retaining the state between them.
 * Missing voltage does not fabricate a new low/not-low observation. */
void battery_low(struct battery_status *b, u16 low_mv, u16 okay_mv)
{
    b->flags &= (u8)~JM_BATTERY_LOW_VALID;
    if (!(b->flags & JM_BATTERY_MV_VALID) || okay_mv < low_mv) return;
    b->flags |= JM_BATTERY_LOW_VALID;
    if (b->mv < low_mv) b->flags |= JM_BATTERY_LOW;
    else if (b->mv >= okay_mv) b->flags &= (u8)~JM_BATTERY_LOW;
}

/* A hardware charging-activity signal does not establish full charge.
 * Boards with no such signal leave CHARGE_VALID clear. */
void battery_charge(struct battery_status *b, int powered, int charging)
{
    b->flags &= (u8)~(JM_BATTERY_EXTERNAL_POWER | JM_BATTERY_CHARGING | JM_BATTERY_FULL);
    b->flags |= JM_BATTERY_CHARGE_VALID;
    if (powered) b->flags |= JM_BATTERY_EXTERNAL_POWER;
    if (powered && charging) b->flags |= JM_BATTERY_CHARGING;
}

#ifdef UNITTEST
int main(void)
{
    struct battery_status b = {0};

    battery_voltage(&b, 3800, 3500, 4100);
    CHECK(b.mv == 3800 && b.percent == 50 && (b.flags & JM_BATTERY_PERCENT_VALID));
    battery_voltage(&b, 3300, 3500, 4100); CHECK(b.percent == 0);
    battery_low(&b, 3500, 3600); CHECK(b.flags & JM_BATTERY_LOW);
    battery_voltage(&b, 3550, 3500, 4100);
    battery_low(&b, 3500, 3600); CHECK(b.flags & JM_BATTERY_LOW);
    battery_voltage(&b, 3600, 3500, 4100);
    battery_low(&b, 3500, 3600); CHECK(!(b.flags & JM_BATTERY_LOW));
    battery_voltage(&b, 4184, 3500, 4100); CHECK(b.percent == 100);
    battery_charge(&b, 1, 1);
    CHECK((b.flags & (JM_BATTERY_EXTERNAL_POWER | JM_BATTERY_CHARGING)) ==
          (JM_BATTERY_EXTERNAL_POWER | JM_BATTERY_CHARGING));
    CHECK(!(b.flags & JM_BATTERY_FULL));
    battery_charge(&b, 1, 0); CHECK(!(b.flags & (JM_BATTERY_CHARGING | JM_BATTERY_FULL)));
    battery_charge(&b, 0, 1); CHECK(!(b.flags & (JM_BATTERY_CHARGING | JM_BATTERY_EXTERNAL_POWER)));
    battery_voltage(&b, 0, 3500, 4100); battery_low(&b, 3500, 3600);
    CHECK(!(b.flags & (JM_BATTERY_MV_VALID | JM_BATTERY_PERCENT_VALID | JM_BATTERY_LOW_VALID)));
    CHECK(b.flags & JM_BATTERY_CHARGE_VALID);
    battery_voltage(&b, 3900, 0, 0);
    CHECK((b.flags & JM_BATTERY_MV_VALID) && !(b.flags & JM_BATTERY_PERCENT_VALID));
    return TEST_DONE("battery");
}
#endif

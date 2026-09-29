#ifndef CYCLONE2_PS_H
#define CYCLONE2_PS_H

/* From an interrupt: note which task it interrupted (CPU share sampling). */
void cyclone2_ps_sample(void);
/* Sample CPU share for `seconds` (hardware timer 2), from zero. */
void cyclone2_ps_sample_for(u32 seconds);
int cyclone2_ps_sampling(void);
/* The task list with state, priority, stack headroom and sampled CPU share. */
void cyclone2_ps_print(void);

#endif

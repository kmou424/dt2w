#ifndef _LINUX_ONETAP2WAKE_H
#define _LINUX_ONETAP2WAKE_H

extern int ot2w_switch;
extern bool ot2w_scr_suspended;
extern bool in_phone_call;

void onetap2wake_setdev(struct input_dev *);

#endif	/* _LINUX_ONETAP2WAKE_H */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KPROFILES_H
#define _KPROFILES_H

#include <linux/notifier.h>

#if IS_ENABLED(CONFIG_KPROFILES)
extern unsigned int KP_MODE_CHANGE;

void kp_set_mode(unsigned int level);
void kp_set_mode_rollback(unsigned int level, unsigned int duration_ms);
int kp_active_mode(void);
int kp_notifier_register_client(struct notifier_block *nb);
int kp_notifier_unregister_client(struct notifier_block *nb);
#else
static inline void kp_set_mode(unsigned int level)
{
}

static inline void kp_set_mode_rollback(unsigned int level, unsigned int duration_ms)
{
}

static inline int kp_active_mode(void)
{
	return 2;
}

static inline int kp_notifier_register_client(struct notifier_block *nb)
{
	return 0;
}

static inline int kp_notifier_unregister_client(struct notifier_block *nb)
{
	return 0;
}
#endif /* CONFIG_KPROFILES */
#endif /* _KPROFILES_H */

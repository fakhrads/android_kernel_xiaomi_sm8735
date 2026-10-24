/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_QCOM_DCVS_BOOST_H__
#define __SOC_QCOM_DCVS_BOOST_H__

#if IS_ENABLED(CONFIG_QCOM_DCVS_BOOST)
void qcom_dcvs_bus_boost_kick(unsigned int duration_ms);
void qcom_dcvs_bus_boost_kick_max(unsigned int duration_ms);
int dcvs_boost_init(void);
void dcvs_boost_exit(void);
#else
static inline void qcom_dcvs_bus_boost_kick(unsigned int duration_ms)
{
}
static inline void qcom_dcvs_bus_boost_kick_max(unsigned int duration_ms)
{
}
static inline int dcvs_boost_init(void)
{
	return 0;
}
static inline void dcvs_boost_exit(void)
{
}
#endif /* CONFIG_QCOM_DCVS_BOOST */
#endif /* __SOC_QCOM_DCVS_BOOST_H__ */

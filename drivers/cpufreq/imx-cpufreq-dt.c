// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2019 NXP
 */

#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/device_cooling.h>

#define OCOTP_CFG3_SPEED_GRADE_SHIFT	8
#define OCOTP_CFG3_SPEED_GRADE_MASK	(0x3 << 8)
#define IMX8MN_OCOTP_CFG3_SPEED_GRADE_MASK	(0xf << 8)
#define OCOTP_CFG3_MKT_SEGMENT_SHIFT    6
#define OCOTP_CFG3_MKT_SEGMENT_MASK     (0x3 << 6)
#define IMX8MP_OCOTP_CFG3_MKT_SEGMENT_SHIFT    5
#define IMX8MP_OCOTP_CFG3_MKT_SEGMENT_MASK     (0x3 << 5)

/* cpufreq-dt device registered by imx-cpufreq-dt */
static struct platform_device *cpufreq_dt_pdev;
static struct opp_table *cpufreq_opp_table;

#define cpu_cooling_core_mask ((1 << 1) | (1 << 2) | (1 << 3))   /* offline cpu1 cpu2 and cpu3 if needed*/
static int thermal_hot_pm_notify(struct notifier_block *nb, unsigned long event,
       void *dummy)
{
	static unsigned long prev_event = 0xffffffff;
	struct device *cpu_dev = NULL;
	static int cpus_offlined = 0;
	int i = 0, ret = 0;

	if (event == prev_event)
		return NOTIFY_OK;

	prev_event = event;

	switch (event) {
	case 0: /* default state, no trip point reached*/
	case 1: /* trip1 temperature are lower than trip2, we can
		   online the cpu1, cpu2 and cpu3 to get better performance */
		for (i = 0; i < num_possible_cpus(); i++) {
			if (!(cpu_cooling_core_mask & BIT(i)))
				continue;
			if (!(cpus_offlined & BIT(i)))
				continue;
			cpus_offlined &= ~BIT(i);
			pr_info("Allow Online CPU%d, devfreq state: %d\n",
					i, event);

			lock_device_hotplug();
			if (cpu_online(i)) {
				unlock_device_hotplug();
				continue;
			}
			cpu_dev = get_cpu_device(i);
			ret = device_online(cpu_dev);
			if (ret)
				pr_err("Error %d online core %d\n",
						ret, i);
			unlock_device_hotplug();
		}
		break;
	case 2: /* rise above trip2 temperature, offline cpu1, cpu2 and cpu3 to
		   to limit the max online cpu cores */
		for (i = num_possible_cpus() - 1; i >= 0; i--) {
			if (!(cpu_cooling_core_mask & BIT(i)))
				continue;
			if (cpus_offlined & BIT(i) && !cpu_online(i))
				continue;
			pr_info("Set Offline: CPU%d, devfreq state: %d\n",
					i, event);
			lock_device_hotplug();
			if (cpu_online(i)) {
				cpu_dev = get_cpu_device(i);
				ret = device_offline(cpu_dev);
				if (ret < 0)
					pr_err("Error %d offline core %d\n",
					       ret, i);
			}
			unlock_device_hotplug();
			cpus_offlined |= BIT(i);
		}
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block thermal_hot_pm_notifier =
{
	.notifier_call = thermal_hot_pm_notify,
};

static int imx_cpufreq_dt_probe(struct platform_device *pdev)
{
	struct device *cpu_dev = get_cpu_device(0);
	u32 cell_value, supported_hw[2];
	int speed_grade, mkt_segment;
	int ret;

	ret = nvmem_cell_read_u32(cpu_dev, "speed_grade", &cell_value);
	if (ret)
		return ret;

	if (of_machine_is_compatible("fsl,imx8mn") || of_machine_is_compatible("fsl,imx8mp"))
		speed_grade = (cell_value & IMX8MN_OCOTP_CFG3_SPEED_GRADE_MASK)
			      >> OCOTP_CFG3_SPEED_GRADE_SHIFT;
	else
		speed_grade = (cell_value & OCOTP_CFG3_SPEED_GRADE_MASK)
			      >> OCOTP_CFG3_SPEED_GRADE_SHIFT;
	if (of_machine_is_compatible("fsl,imx8mp"))
		mkt_segment = (cell_value & IMX8MP_OCOTP_CFG3_MKT_SEGMENT_MASK)
			       >> IMX8MP_OCOTP_CFG3_MKT_SEGMENT_SHIFT;
	else
		mkt_segment = (cell_value & OCOTP_CFG3_MKT_SEGMENT_MASK)
			       >> OCOTP_CFG3_MKT_SEGMENT_SHIFT;

	/*
	 * Early samples without fuses written report "0 0" which may NOT
	 * match any OPP defined in DT. So clamp to minimum OPP defined in
	 * DT to avoid warning for "no OPPs".
	 *
	 * Applies to i.MX8M series SoCs.
	 */
	if (mkt_segment == 0 && speed_grade == 0) {
		if (of_machine_is_compatible("fsl,imx8mm") ||
		    of_machine_is_compatible("fsl,imx8mq"))
			speed_grade = 1;
		if (of_machine_is_compatible("fsl,imx8mn") ||
			of_machine_is_compatible("fsl,imx8mp"))
			speed_grade = 0xb;
	}

	supported_hw[0] = BIT(speed_grade);
	supported_hw[1] = BIT(mkt_segment);
	dev_info(&pdev->dev, "cpu speed grade %d mkt segment %d supported-hw %#x %#x\n",
			speed_grade, mkt_segment, supported_hw[0], supported_hw[1]);

	cpufreq_opp_table = dev_pm_opp_set_supported_hw(cpu_dev, supported_hw, 2);
	if (IS_ERR(cpufreq_opp_table)) {
		ret = PTR_ERR(cpufreq_opp_table);
		dev_err(&pdev->dev, "Failed to set supported opp: %d\n", ret);
		return ret;
	}

	cpufreq_dt_pdev = platform_device_register_data(
			&pdev->dev, "cpufreq-dt", -1, NULL, 0);
	if (IS_ERR(cpufreq_dt_pdev)) {
		dev_pm_opp_put_supported_hw(cpufreq_opp_table);
		ret = PTR_ERR(cpufreq_dt_pdev);
		dev_err(&pdev->dev, "Failed to register cpufreq-dt: %d\n", ret);
		return ret;
	}

	register_devfreq_cooling_notifier(&thermal_hot_pm_notifier);

	return 0;
}

static int imx_cpufreq_dt_remove(struct platform_device *pdev)
{
	unregister_devfreq_cooling_notifier(&thermal_hot_pm_notifier);
	platform_device_unregister(cpufreq_dt_pdev);
	dev_pm_opp_put_supported_hw(cpufreq_opp_table);

	return 0;
}

static struct platform_driver imx_cpufreq_dt_driver = {
	.probe = imx_cpufreq_dt_probe,
	.remove = imx_cpufreq_dt_remove,
	.driver = {
		.name = "imx-cpufreq-dt",
	},
};
module_platform_driver(imx_cpufreq_dt_driver);

MODULE_ALIAS("platform:imx-cpufreq-dt");
MODULE_DESCRIPTION("Freescale i.MX cpufreq speed grading driver");
MODULE_LICENSE("GPL v2");

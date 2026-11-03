/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT sekigon_dynamic_macro_manager

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

static int macro_manager_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

#define MACRO_MANAGER_INST(inst)                                                                   \
    DEVICE_DT_INST_DEFINE(inst, macro_manager_init, NULL, NULL, NULL, POST_KERNEL,                 \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);

DT_INST_FOREACH_STATUS_OKAY(MACRO_MANAGER_INST)

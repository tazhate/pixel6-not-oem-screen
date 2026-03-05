// SPDX-License-Identifier: GPL-2.0
/* Minimal test module — just loads and prints */
#include <linux/module.h>
#include <linux/kernel.h>

static int __init test_init(void)
{
	pr_info("test_module: loaded successfully\n");
	return 0;
}

static void __exit test_exit(void)
{
	pr_info("test_module: unloaded\n");
}

module_init(test_init);
module_exit(test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal test module");

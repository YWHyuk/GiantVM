#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x2f1b9ab7, "module_layout" },
	{ 0x7c2a1d8a, "debugfs_create_dir" },
	{ 0x3cbab137, "seq_open" },
	{ 0x76a3e93f, "seq_printf" },
	{ 0x837b7b09, "__dynamic_pr_debug" },
	{ 0xeae3dfd6, "__const_udelay" },
	{ 0xb5fbfccf, "debugfs_create_file" },
	{ 0x7a2af7b4, "cpu_number" },
	{ 0xe2ef2029, "seq_read" },
	{ 0x539f3022, "pv_ops" },
	{ 0x303a7d56, "kthread_create_on_node" },
	{ 0xaa44a707, "cpumask_next" },
	{ 0xc4c583e2, "kthread_bind" },
	{ 0x17de3d5, "nr_cpu_ids" },
	{ 0xd35cce70, "_raw_spin_unlock_irqrestore" },
	{ 0xc5850110, "printk" },
	{ 0x5a5a2271, "__cpu_online_mask" },
	{ 0xe1f8521e, "debugfs_remove" },
	{ 0xf7d31de9, "kstrtoul_from_user" },
	{ 0xc959d152, "__stack_chk_fail" },
	{ 0xb8b9f817, "kmalloc_order_trace" },
	{ 0x2ea2c95c, "__x86_indirect_thunk_rax" },
	{ 0x3a26ed11, "sched_clock" },
	{ 0xf23af562, "wake_up_process" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xb19a5453, "__per_cpu_offset" },
	{ 0x34db050b, "_raw_spin_lock_irqsave" },
	{ 0x18a8b57e, "seq_lseek" },
	{ 0x37a0cba, "kfree" },
	{ 0x53569707, "this_cpu_off" },
	{ 0x9a353ae, "__x86_indirect_alt_call_rax" },
	{ 0x43606eb1, "seq_release" },
	{ 0xa084f79f, "cpumask_next_wrap" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "D7235BACD8127030210D7C0");

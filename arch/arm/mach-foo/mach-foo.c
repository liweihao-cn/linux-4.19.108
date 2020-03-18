/* mach-foo.c */
#include <linux/io.h>
#include <linux/compiler.h>

#include <asm/mach/arch.h>

static void __init foo_machine_init(void)
{
}

static const __initconst char *const foo_dt_compat[] = {
	"none,foo",
	NULL,
};

DT_MACHINE_START(foo, "mach-foo(DT)")
	.init_machine	= foo_machine_init,
	.dt_compat	= foo_dt_compat,
MACHINE_END
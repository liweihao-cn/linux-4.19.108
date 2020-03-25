
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sched_clock.h>

#include "timer-of.h"

#define TCFG0			0x00
#define TCFG1			0x04
#define TCON			0x08

#define TCNTB(id)		(0x0c + 12 * (id))
#define TCMPB(id)		(0x10 + 12 * (id))

#define _TCON_START(id) 	(1 << (4 * (id - 1) + 8))
#define _TCON_START0(id)	(1)
#define TCON_START(id)		\
	((id == 0) ? _TCON_START0(id) : _TCON_START(id))

#define _TCON_MANUALUPDATE(id)	(1 << (4 * (id - 1) + 9))
#define _TCON_MANUALUPDATE0(id)	(1 << 1)
#define TCON_MANUALUPDATE(id)	\
	((id == 0) ? _TCON_MANUALUPDATE0(id) : _TCON_MANUALUPDATE(id))

#define _TCON_AUTORELOAD(id)	(1 << (4 * (id - 1) + 11))
#define _TCON_AUTORELOAD0(id)	(1 << 3)
#define _TCON_AUTORELOAD4(id)	(1 << 22)
#define TCON_AUTORELOAD(id)	\
	((id == 0) ? _TCON_AUTORELOAD0(id) : \
	((id == 4) ? _TCON_AUTORELOAD4(id) : _TCON_AUTORELOAD(id)))

struct foo_timer {
	u32 id;
	void __iomem *source;
};

void __iomem *clock_source;

static void foo_timer_hw_stop(struct timer_of *to)
{
	struct foo_timer *timer = to->private_data;
	void __iomem *base = timer_of_base(to);
	u32 val = readl_relaxed(base + TCON);

	val &= ~TCON_START(timer->id);
	writel_relaxed(val, base + TCON);
}

static void foo_timer_hw_start(struct timer_of *to, bool period)
{
	struct foo_timer *timer = to->private_data;
	void __iomem *base = timer_of_base(to);
	u32 val = readl_relaxed(base + TCON);

	val &= ~TCON_MANUALUPDATE(timer->id);
	val |= TCON_START(timer->id);
	if (period)
		val |= TCON_AUTORELOAD(timer->id);
	writel_relaxed(val, base + TCON);
}

static void foo_timer_hw_set_count(struct timer_of *to, unsigned long count)
{
	struct foo_timer *timer = to->private_data;
	void __iomem *base = timer_of_base(to);
	u32 val = readl_relaxed(base + TCON);

	val &= ~(TCON_START(timer->id) | TCON_AUTORELOAD(timer->id));
	val |= TCON_MANUALUPDATE(timer->id);
	writel_relaxed(val, base + TCON);

	writel_relaxed(count, base + TCNTB(timer->id));
	if (timer->id != 4)
		writel_relaxed(count, base + TCMPB(timer->id));
}

static void __init foo_timer_set_prescaler(struct timer_of *to)
{
	struct foo_timer *timer = to->private_data;
	u32 prescaler_mask, prescaler_shift;
	u32 val, temp; 
	
	if (timer->id > 1) {
		prescaler_shift = 8;
		prescaler_mask = 0x0000ff00;
	} else {
		prescaler_shift = 0;
		prescaler_mask = 0x000000ff;
	}
	
	val = readl(timer_of_base(to) + TCFG0);
	temp = (val & prescaler_mask) >> prescaler_shift;
	if (temp != 25) {
		val &= ~prescaler_mask;
		val |= (25 << prescaler_shift);
		writel(val, timer_of_base(to) + TCFG0);
	}
}

static void __init foo_timer_set_mux(struct timer_of *to)
{
	struct foo_timer *timer = to->private_data;
	u32 mux_mask, mux_shift;
	u32 val, temp;

	mux_mask = 0xf << timer->id;
	mux_shift = 4 * timer->id;

	val = readl(timer_of_base(to) + TCFG1);
	temp = (val & mux_mask) >> mux_shift;
	if (temp != 0) {
		val &= ~mux_mask;
		writel(val, timer_of_base(to) + TCFG1);
	}
}

static u64 notrace foo_timer_sched_read(void)
{
	return ~(u64)readl_relaxed(clock_source);
}

static irqreturn_t foo_clockevent_isr(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static int foo_clock_event_shutdown(struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	foo_timer_hw_stop(to);

	return 0;
}

static int foo_clock_event_set_oneshot(struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	foo_timer_hw_stop(to);
	foo_timer_hw_start(to, 0);

	return 0;
}

static int foo_clock_event_set_periodic(struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	foo_timer_hw_set_count(to, timer_of_period(to));
	foo_timer_hw_start(to, 1);

	return 0;
}

static int foo_clock_event_set_next_event(unsigned long evt,
					  struct clock_event_device *clkevt)
{
	struct timer_of *to = to_timer_of(clkevt);

	foo_timer_hw_set_count(to, evt);
	foo_timer_hw_start(to, 0);

	return 0;
}

static int __init foo_clock_source_init(struct timer_of *to)
{
	int ret;
	struct foo_timer *timer = to->private_data;

	clock_source = timer->source;
	sched_clock_register(foo_timer_sched_read, 16, timer_of_rate(to));
	ret = clocksource_mmio_init(clock_source, to->np->full_name,
				    timer_of_rate(to), 200, 16,
				    clocksource_mmio_readl_down);
	foo_timer_hw_set_count(to, 0xffff);
	foo_timer_hw_start(to, 1);

	return ret;
}

static int __init foo_clock_event_init(struct timer_of *to)
{
	to->clkevt.name = to->np->full_name;
	to->clkevt.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	to->clkevt.set_state_shutdown = foo_clock_event_shutdown;
	to->clkevt.set_state_periodic = foo_clock_event_set_periodic;
	to->clkevt.set_state_oneshot = foo_clock_event_set_oneshot;
	to->clkevt.tick_resume = foo_clock_event_shutdown;
	to->clkevt.set_next_event = foo_clock_event_set_next_event;
	to->clkevt.rating = 200;

	clockevents_config_and_register(&to->clkevt, timer_of_rate(to),
					0x1, 0xffff);

	foo_timer_hw_set_count(to, timer_of_period(to));
	foo_timer_hw_start(to, 1);

	return 0;
}

static int __init foo_timer_init(struct device_node *node)
{
	int ret, event_device = 0;
	struct timer_of *to;
	struct foo_timer *timer;

	to = kzalloc(sizeof(*to), GFP_KERNEL);
	if (!to)
		return -ENOMEM;

	to->flags = TIMER_OF_CLOCK | TIMER_OF_BASE;

	/* if (of_find_property(node, "clock-source-device", NULL)) {
		pr_info("register %s as a clock source device\n",
			node->full_name);
	} */

	if (of_find_property(node, "clock-event-device", NULL)) {
		to->flags |= TIMER_OF_IRQ;
		to->of_irq.handler = foo_clockevent_isr;
		to->of_irq.flags = IRQF_TIMER | IRQF_IRQPOLL;
		event_device = 1;
	}

	ret = timer_of_init(node, to);
	if (ret)
		return ret;

	/* private data init */
	timer = kzalloc(sizeof(struct foo_timer), GFP_KERNEL);
	if (!timer) {
		kfree(to);
		return -ENOMEM;
	}

	of_property_read_u32(node, "timer-id", &timer->id);
	if (timer->id == 4) {
		timer->source = timer_of_base(to) + TCNTB(4) + 4;
	} else {
		timer->source = timer_of_base(to) + TCMPB(4) + 4;
	}
	to->private_data = timer;

	foo_timer_set_prescaler(to);
	foo_timer_set_mux(to);
	to->of_clk.rate = DIV_ROUND_CLOSEST(to->of_clk.rate, 50);
	to->of_clk.period = DIV_ROUND_UP(to->of_clk.rate, HZ);

	if (event_device) {
		return foo_clock_event_init(to);
	} else {
		return foo_clock_source_init(to);
	}

	return 0;
}
TIMER_OF_DECLARE(foo, "foo,timer", foo_timer_init);

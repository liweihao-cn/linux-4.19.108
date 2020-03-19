#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <asm/exception.h>

#define FOO_INTC_NAME_MAIN	"foo-mic"
#define FOO_INTC_NAME_SUB	"foo-sic"

#define NR_FOO_MIC_IRQS		32
#define NR_FOO_SIC_IRQS		15

#define FOO_MIC_PENDING		0x00
#define FOO_MIC_MASK		0x08
#define FOO_MIC_INTPND		0x10

#define FOO_SIC_PENDING		0x18
#define FOO_SIC_MASK		0x1c

struct foo_irq_data {
	unsigned long irq;
	unsigned long hw;
	unsigned long sub_bits;

	unsigned long parent_irq;
	unsigned long parent_hw;
};

struct foo_irq_intc {
	void __iomem		*base;
	struct irq_domain	*domain;
	struct irq_chip		chip;
	struct foo_irq_data	*irq_data;
	struct foo_irq_intc	*parent;
};

static struct foo_irq_intc *foo_intc_mic;

static int sic_get_parent_hw(int sic_hw)
{
	int parent_hw;
	switch (sic_hw) {
	case 0:
	case 1:
	case 2:
		parent_hw = 28;
		break;
	case 3:
	case 4:
	case 5:
		parent_hw = 23;
		break;
	case 6:
	case 7:
	case 8:
		parent_hw = 15;
		break;
	case 9:
	case 10:
		parent_hw = 31;
		break;
	case 11:
	case 12:
		parent_hw = 6;
		break;
	case 13:
	case 14:
		parent_hw = 9;
		break;
	}
	return parent_hw;
}

static void foo_irq_ack(struct irq_data *d)
{
	struct foo_irq_intc *intc = irq_data_get_irq_chip_data(d);
	struct foo_irq_intc *parent_intc = intc->parent;
	u32 val = BIT(d->hwirq);
	int parent_hw;

	if (parent_intc) {
		writel_relaxed(val, intc->base + FOO_SIC_PENDING);
		parent_hw = sic_get_parent_hw(d->hwirq);
		val = BIT(parent_hw);
		writel_relaxed(val, intc->base + FOO_MIC_PENDING);
		writel_relaxed(val, intc->base + FOO_MIC_INTPND);
	} else {
		writel_relaxed(val, intc->base + FOO_MIC_PENDING);
		writel_relaxed(val, intc->base + FOO_MIC_INTPND);
	}
}

static void foo_irq_mask(struct irq_data *d)
{
	struct foo_irq_intc *intc = irq_data_get_irq_chip_data(d);
	struct foo_irq_intc *parent_intc = intc->parent;
	struct foo_irq_data *parent_irq_data;
	u32 val, mask = BIT(d->hwirq);
	int parent_hw, parent_sub_bits;

	if (parent_intc) {
		val = readl_relaxed(intc->base + FOO_SIC_MASK) | mask;
		writel_relaxed(val, intc->base + FOO_SIC_MASK);
		parent_hw = sic_get_parent_hw(d->hwirq);
		parent_irq_data = &parent_intc->irq_data[parent_hw];
		parent_sub_bits = parent_irq_data->sub_bits;
		if ((parent_sub_bits & val) == parent_sub_bits) {
			val = readl_relaxed(intc->base + FOO_MIC_MASK);
			val |= BIT(parent_hw);
			writel_relaxed(val, intc->base + FOO_MIC_MASK);
		}
	} else {
		val = readl_relaxed(intc->base + FOO_MIC_MASK) | mask;
		writel_relaxed(val, intc->base + FOO_MIC_MASK);
	}
}

static void foo_irq_unmask(struct irq_data *d)
{
	struct foo_irq_intc *intc = irq_data_get_irq_chip_data(d);
	struct foo_irq_intc *parent_intc = intc->parent;
	unsigned long mask;
	int parent_hw;

	if (parent_intc) {
		mask = readl_relaxed(intc->base + FOO_SIC_MASK);
		mask &= ~(BIT(d->hwirq));
		writel_relaxed(mask, intc->base + FOO_SIC_MASK);
		parent_hw = sic_get_parent_hw(d->hwirq);
		mask = readl_relaxed(intc->base + FOO_MIC_MASK);
		mask &= ~(BIT(parent_hw));
		writel_relaxed(mask, intc->base + FOO_MIC_MASK);
	} else {
		mask = readl_relaxed(intc->base + FOO_MIC_MASK);
		mask &= ~(BIT(d->hwirq));
		writel_relaxed(mask, intc->base + FOO_MIC_MASK);
	}
}

static int foo_irq_set_type(struct irq_data *d, unsigned int type)
{
	return 0;
}

static void __exception_irq_entry foo_mic_handle(struct pt_regs *regs)
{
	u32 hwirq = readl_relaxed(foo_intc_mic->base + FOO_MIC_INTPND);
	u32 irq;

	while (hwirq) {
		irq = __ffs(hwirq);
		hwirq &= ~BIT(irq);
		handle_domain_irq(foo_intc_mic->domain, irq, regs);
	}
}

static void foo_sic_handle(struct irq_desc *desc)
{
	struct foo_irq_intc *intc = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 hwirq = readl_relaxed(intc->base + FOO_SIC_PENDING), irq;

	chained_irq_enter(chip, desc);

	while (hwirq) {
		irq = __ffs(hwirq);
		hwirq &= ~BIT(irq);
		generic_handle_irq(irq_find_mapping(intc->domain, irq));
	}

	chained_irq_exit(chip, desc);
}

static int foo_irq_domain_map_of(struct irq_domain *id, unsigned int virq,
				 irq_hw_number_t hw)
{
	struct foo_irq_intc *intc = id->host_data;
	struct foo_irq_intc *parent_intc = intc->parent;
	struct foo_irq_data *irq_data = &intc->irq_data[hw];
	struct foo_irq_data *parent_irq_data;
	int parent_hw;

	irq_set_chip_data(virq, intc);
	irq_set_chip_and_handler(virq, &intc->chip, handle_level_irq);
	irq_set_status_flags(virq, IRQ_LEVEL);
	irq_set_noprobe(virq);

	irq_data->irq = virq;
	irq_data->hw  = hw;

	if (parent_intc) {
		parent_hw = sic_get_parent_hw(hw);
		parent_irq_data = &parent_intc->irq_data[parent_hw];
		parent_irq_data->sub_bits |= (1UL << hw);
		irq_data->parent_irq = parent_irq_data->irq;
		irq_data->parent_hw = parent_hw;
		pr_info("%s hw %d set sub bit %ld\n", parent_intc->chip.name,
			parent_hw, hw);
	}

	return 0;
}

static const struct irq_domain_ops foo_irq_domain_ops_of = {
	.map	= foo_irq_domain_map_of,
	.xlate	= irq_domain_xlate_twocell,
};

static int __init foo_intc_setup_of(struct device_node *node,
				 struct device_node *parent)
{
	struct foo_irq_intc *intc;
	bool is_mic = of_device_is_compatible(node, "foo,foo-mic");
	u32 parent_irq, i, nr_irq;
	
	intc = kzalloc(sizeof(struct foo_irq_intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;

	intc->base = of_iomap(node, 0);
	if (!intc->base) {
		pr_err("%s: unable to map registers\n", node->full_name);
		kfree(intc);
		return -EINVAL;
	}

	intc->chip.irq_ack = foo_irq_ack;
	intc->chip.irq_mask = foo_irq_mask;
	intc->chip.irq_unmask = foo_irq_unmask;
	intc->chip.irq_set_type = foo_irq_set_type;
	if (is_mic) {
		intc->chip.name = FOO_INTC_NAME_MAIN;
		nr_irq = NR_FOO_MIC_IRQS;
	} else {
		intc->chip.name = FOO_INTC_NAME_SUB;
		nr_irq = NR_FOO_SIC_IRQS;
	}

	intc->domain = irq_domain_add_linear(node, nr_irq,
					     &foo_irq_domain_ops_of, intc);
	if (!intc->domain) {
		pr_err("unable to add irq domain\n");
		iounmap(intc->base);
		kfree(intc);
		return -ENODEV;
	}

	intc->irq_data = kcalloc(nr_irq, sizeof(struct foo_irq_data),
				 GFP_KERNEL);
	if (!intc->irq_data) {
		pr_err("unable to alloc irq_data\n");
		irq_domain_remove(intc->domain);
		iounmap(intc->base);
		kfree(intc);
		return -ENOMEM;
	}

	if (is_mic) {
		foo_intc_mic = intc;
		set_handle_irq(foo_mic_handle);
	} else {
		intc->parent = foo_intc_mic;
		for (i = 0; i < of_irq_count(node); i++) {
			parent_irq = irq_of_parse_and_map(node, i);
			if (parent_irq)
				irq_set_chained_handler_and_data(parent_irq,
						      foo_sic_handle, intc);
		}
	}

	return 0;
}

IRQCHIP_DECLARE(foo_mic, "foo,foo-mic", foo_intc_setup_of);
IRQCHIP_DECLARE(foo_sic, "foo,foo-sic", foo_intc_setup_of);

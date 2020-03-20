#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define GATES_REG	0x0c

static DEFINE_SPINLOCK(gates_lock);

static const __initconst char* foo_critical_clocks[] = {
	"uart0",
};

static void __init foo_clk_init(struct device_node *node)
{
	void __iomem *base;
	struct clk_onecell_data *clk_data;
	const char *clk_parent_name, *clk_name;
	struct property *prop;
	struct resource res;
	const __be32 *p;
	struct clk *clk;
	int number, i = 0, j;
	u32 index;

	base = of_io_request_and_map(node, 0, of_node_full_name(node));
	if (IS_ERR(base))
		return;
	
	clk_data = kmalloc(sizeof(struct clk_onecell_data), GFP_KERNEL);
	if (!clk_data)
		goto err_unmap;

	number = of_property_count_u32_elems(node, "clock-indices");
	of_property_read_u32_index(node, "clock-indices", number - 1, &number);

	clk_data->clks = kcalloc(number + 1, sizeof(struct clk *), GFP_KERNEL);
	if (!clk_data->clks)
		goto err_free_data;

	of_property_for_each_u32(node, "clock-indices", prop, p, index) {
		of_property_read_string_index(node, "clock-parent-names",
					      i, &clk_parent_name);
		of_property_read_string_index(node, "clock-gates-names",
					      i, &clk_name);

		clk_data->clks[index] = clk_register_gate(NULL, clk_name,
							  clk_parent_name, 0,
							  base + GATES_REG,
							  index,
							  0, &gates_lock);
		i++;

		if (IS_ERR(clk_data->clks[index])) {
			WARN_ON(true);
			continue;
		}

		for (j = 0; j < ARRAY_SIZE(foo_critical_clocks); j++)
		{
			clk = __clk_lookup(foo_critical_clocks[j]);
			if (clk)
				clk_prepare_enable(clk);
		}
	}

	clk_data->clk_num = number + 1;
	of_clk_add_provider(node, of_clk_src_onecell_get, clk_data);

	return;

err_free_data:
	kfree(clk_data);
err_unmap:
	iounmap(base);
	of_address_to_resource(node, 0, &res);
	release_mem_region(res.start, resource_size(&res));
}
CLK_OF_DECLARE(foo_clk, "foo,foo-clock", foo_clk_init);

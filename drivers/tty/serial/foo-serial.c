#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/serial_core.h>

#include <asm/io.h>
#include <asm/early_ioremap.h>

#define UART_UTRSTAT	(0x10)
#define UART_UTXH	(0x20)

static void foo_serial_putc(struct uart_port *port, int ch)
{
	while (!(readl(port->membase + UART_UTRSTAT) & 0x2));
	writeb(ch, port->membase + UART_UTXH);
}

static void earlycon_foo_write(struct console *con, const char *s, unsigned n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, foo_serial_putc);
}

int __init earlycon_foo_setup(struct earlycon_device *dev, const char *opt)
{
	if (!dev->port.membase)
		return -ENODEV;

	dev->con->write = earlycon_foo_write;
	return 0;
}
EARLYCON_DECLARE(foo_serial, earlycon_foo_setup);
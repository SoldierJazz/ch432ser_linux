/*
 * SPI serial driver for SPI to Dual UARTs chip ch432.
 *
 * Copyright (C) 2024 Nanjing Qinheng Microelectronics Co., Ltd.
 * Web:      http://wch.cn
 * Author:   WCH <tech@wch.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Update Log:
 * V1.0 - initial version
 * V1.1 - fix bugs in ch43x_start_tx
 * V1.2 - fix high baud rates setting bugs
 *      - change fifo trigger to 8
 *      - add receive timeout handling
 *      - add support of hardflow setting
 * V1.3 - modify rs485 configuration in ioctl, add support for sysfs debug
 */

#define DEBUG
#define VERBOSE_DEBUG

#undef DEBUG
#undef VERBOSE_DEBUG

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/spi/spi.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/serial_reg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>
#include <linux/irq.h>
#include "linux/version.h"

#define DRIVER_AUTHOR "WCH"
#define DRIVER_DESC   "SPI serial driver for ch432."
#define VERSION_DESC  "V1.3 On 2024.07"

#ifndef PORT_SC16IS7XX
#define PORT_SC16IS7XX 128
#endif

/* external crystal freq */
#define CRYSTAL_FREQ 22118400

#define USE_IRQ_FROM_DTS
#define GPIO_NUMBER 0
#define USE_SPI_MODE
#define CH43X_NAME     "ch43x"
#define CH43X_NAME_SPI "ch43x_spi"

/* CH43X register definitions */
#define CH43X_RHR_REG (0x00) /* RX FIFO */
#define CH43X_THR_REG (0x00) /* TX FIFO */
#define CH43X_IER_REG (0x01) /* Interrupt enable */
#define CH43X_IIR_REG (0x02) /* Interrupt Identification */
#define CH43X_FCR_REG (0x02) /* FIFO control */
#define CH43X_LCR_REG (0x03) /* Line Control */
#define CH43X_MCR_REG (0x04) /* Modem Control */
#define CH43X_LSR_REG (0x05) /* Line Status */
#define CH43X_MSR_REG (0x06) /* Modem Status */
#define CH43X_SPR_REG (0x07) /* Scratch Pad */

/* Special Register set: Only if (LCR[7] == 1) */
#define CH43X_DLL_REG (0x00) /* Divisor Latch Low */
#define CH43X_DLH_REG (0x01) /* Divisor Latch High */

/* IER register bits */
#define CH43X_IER_RDI_BIT  (1 << 0) /* Enable RX data interrupt */
#define CH43X_IER_THRI_BIT (1 << 1) /* Enable TX holding register interrupt */
#define CH43X_IER_RLSI_BIT (1 << 2) /* Enable RX line status interrupt */
#define CH43X_IER_MSI_BIT  (1 << 3) /* Enable Modem status interrupt */

/* IER enhanced register bits */
#define CH43X_IER_RESET_BIT    (1 << 7) /* Enable Soft reset */
#define CH43X_IER_LOWPOWER_BIT (1 << 6) /* Enable low power mode */
#define CH43X_IER_SLEEP_BIT    (1 << 5) /* Enable sleep mode */
#define CH43X_IER_CK2X_BIT     (1 << 5) /* Enable clk * 2 */

/* FCR register bits */
#define CH43X_FCR_FIFO_BIT    (1 << 0) /* Enable FIFO */
#define CH43X_FCR_RXRESET_BIT (1 << 1) /* Reset RX FIFO */
#define CH43X_FCR_TXRESET_BIT (1 << 2) /* Reset TX FIFO */
#define CH43X_FCR_RXLVLL_BIT  (1 << 6) /* RX Trigger level LSB */
#define CH43X_FCR_RXLVLH_BIT  (1 << 7) /* RX Trigger level MSB */

/* IIR register bits */
#define CH43X_IIR_NO_INT_BIT (1 << 0) /* No interrupts pending */
#define CH43X_IIR_ID_MASK    0x0e     /* Mask for the interrupt ID */
#define CH43X_IIR_THRI_SRC   0x02     /* TX holding register empty */
#define CH43X_IIR_RDI_SRC    0x04     /* RX data interrupt */
#define CH43X_IIR_RLSE_SRC   0x06     /* RX line status error */
#define CH43X_IIR_RTOI_SRC   0x0c     /* RX time-out interrupt */
#define CH43X_IIR_MSI_SRC    0x00     /* Modem status interrupt */

/* LCR register bits */
#define CH43X_LCR_LENGTH0_BIT (1 << 0) /* Word length bit 0 */
#define CH43X_LCR_LENGTH1_BIT \
	(1 << 1) /* Word length bit 1
						  *
						  * Word length bits table:
						  * 00 -> 5 bit words
						  * 01 -> 6 bit words
						  * 10 -> 7 bit words
						  * 11 -> 8 bit words
						  */
#define CH43X_LCR_STOPLEN_BIT \
	(1 << 2)			   /* STOP length bit
						  *
						  * STOP length bit table:
						  * 0 -> 1 stop bit
						  * 1 -> 1-1.5 stop bits if
						  *      word length is 5,
						  *      2 stop bits otherwise
						  */
#define CH43X_LCR_PARITY_BIT	  (1 << 3) /* Parity bit enable */
#define CH43X_LCR_ODDPARITY_BIT	  (0)	   /* Odd parity bit enable */
#define CH43X_LCR_EVENPARITY_BIT  (1 << 4) /* Even parity bit enable */
#define CH43X_LCR_MARKPARITY_BIT  (1 << 5) /* Mark parity bit enable */
#define CH43X_LCR_SPACEPARITY_BIT (3 << 4) /* Space parity bit enable */

#define CH43X_LCR_TXBREAK_BIT (1 << 6) /* TX break enable */
#define CH43X_LCR_DLAB_BIT    (1 << 7) /* Divisor Latch enable */
#define CH43X_LCR_WORD_LEN_5  (0x00)
#define CH43X_LCR_WORD_LEN_6  (0x01)
#define CH43X_LCR_WORD_LEN_7  (0x02)
#define CH43X_LCR_WORD_LEN_8  (0x03)
#define CH43X_LCR_CONF_MODE_A CH43X_LCR_DLAB_BIT /* Special reg set */

/* MCR register bits */
#define CH43X_MCR_DTR_BIT  (1 << 0) /* DTR complement */
#define CH43X_MCR_RTS_BIT  (1 << 1) /* RTS complement */
#define CH43X_MCR_OUT1	   (1 << 2) /* OUT1 */
#define CH43X_MCR_OUT2	   (1 << 3) /* OUT2 */
#define CH43X_MCR_LOOP_BIT (1 << 4) /* Enable loopback test mode */
#define CH43X_MCR_AFE	   (1 << 5) /* Enable Hardware Flow control */

/* LSR register bits */
#define CH43X_LSR_DR_BIT	 (1 << 0) /* Receiver data ready */
#define CH43X_LSR_OE_BIT	 (1 << 1) /* Overrun Error */
#define CH43X_LSR_PE_BIT	 (1 << 2) /* Parity Error */
#define CH43X_LSR_FE_BIT	 (1 << 3) /* Frame Error */
#define CH43X_LSR_BI_BIT	 (1 << 4) /* Break Interrupt */
#define CH43X_LSR_BRK_ERROR_MASK 0x1E	  /* BI, FE, PE, OE bits */
#define CH43X_LSR_THRE_BIT	 (1 << 5) /* TX holding register empty */
#define CH43X_LSR_TEMT_BIT	 (1 << 6) /* Transmitter empty */
#define CH43X_LSR_FIFOE_BIT	 (1 << 7) /* Fifo Error */

/* MSR register bits */
#define CH43X_MSR_DCTS_BIT   (1 << 0) /* Delta CTS Clear To Send */
#define CH43X_MSR_DDSR_BIT   (1 << 1) /* Delta DSR Data Set Ready */
#define CH43X_MSR_DRI_BIT    (1 << 2) /* Delta RI Ring Indicator */
#define CH43X_MSR_DCD_BIT    (1 << 3) /* Delta CD Carrier Detect */
#define CH43X_MSR_CTS_BIT    (1 << 4) /* CTS */
#define CH43X_MSR_DSR_BIT    (1 << 5) /* DSR */
#define CH43X_MSR_RI_BIT     (1 << 6) /* RI */
#define CH43X_MSR_CD_BIT     (1 << 7) /* CD */
#define CH43X_MSR_DELTA_MASK 0x0F     /* Any of the delta bits! */

/* Misc definitions */
#define CH43X_FIFO_SIZE (16)
#define CH43X_REG_SHIFT 2

#define IOCTL_MAGIC	 'W'
#define IOCTL_CMD_GRS485 _IOR(IOCTL_MAGIC, 0x86, uint16_t)
#define IOCTL_CMD_SRS485 _IOW(IOCTL_MAGIC, 0x87, uint16_t)

struct ch43x_devtype {
	char name[10];
	int nr_uart;
};

struct ch43x_one {
	struct uart_port port;
	struct work_struct tx_work;
	struct work_struct md_work;
	struct work_struct stop_rx_work;
	struct work_struct stop_tx_work;
	struct serial_rs485 rs485;
	unsigned char msr_reg;
	unsigned char ier;
	unsigned char mcr_force;
};

struct ch43x_port {
	struct uart_driver uart;
	struct ch43x_devtype *devtype;
	struct mutex mutex;
	struct mutex mutex_bus_access;
	struct clk *clk;
	struct spi_device *spi_dev;
	unsigned char buf[65536];
	struct ch43x_one p[0];
};

struct ch43x_port *g_ch43x_port;

#define to_ch43x_one(p, e) ((container_of((p), struct ch43x_one, e)))
#ifdef USE_SPI_MODE
static u8 ch43x_port_read(struct uart_port *port, u8 reg)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	unsigned char cmd;
	ssize_t status;
	u8 result;

	mutex_lock(&s->mutex_bus_access);
	cmd = 0xFD & ((reg + port->line * 0x08) << CH43X_REG_SHIFT);

	status = spi_write_then_read(s->spi_dev, &cmd, 1, &result, 1);
	mutex_unlock(&s->mutex_bus_access);
	if (status < 0) {
		dev_err(&s->spi_dev->dev, "Failed to ch43x_port_read error code %ld\n", (unsigned long)status);
	}
	dev_vdbg(&s->spi_dev->dev, "%s - reg:0x%2x, data:0x%2x\n", __func__, reg + port->line * 0x08, result);

	return result;
}

static u8 ch43x_port_read_specify(struct uart_port *port, u8 portnum, u8 reg)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	unsigned char cmd;
	ssize_t status;
	u8 result;

	mutex_lock(&s->mutex_bus_access);
	cmd = 0xFD & ((reg + portnum * 0x08) << CH43X_REG_SHIFT);

	status = spi_write_then_read(s->spi_dev, &cmd, 1, &result, 1);
	mutex_unlock(&s->mutex_bus_access);
	if (status < 0) {
		dev_err(&s->spi_dev->dev, "Failed to ch43x_port_read error code %ld\n", (unsigned long)status);
	}
	dev_vdbg(&s->spi_dev->dev, "%s - reg:0x%2x, data:0x%2x\n", __func__, reg + portnum * 0x08, result);

	return result;
}

static void ch43x_port_write(struct uart_port *port, u8 reg, u8 val)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	unsigned char spi_buf[2];
	ssize_t status;

	mutex_lock(&s->mutex_bus_access);
	spi_buf[0] = 0x02 | ((reg + port->line * 0x08) << CH43X_REG_SHIFT);

	spi_buf[1] = val;

	status = spi_write(s->spi_dev, spi_buf, 2);
	if (status < 0) {
		dev_err(&s->spi_dev->dev, "Failed to ch43x_port_write Err_code %ld\n", (unsigned long)status);
	}
	mutex_unlock(&s->mutex_bus_access);

	dev_vdbg(&s->spi_dev->dev, "%s - reg:0x%2x, data:0x%2x\n", __func__, reg + port->line * 0x08, spi_buf[1]);
}

static void ch43x_port_write_spefify(struct uart_port *port, u8 portnum, u8 reg, u8 val)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	unsigned char spi_buf[2];
	ssize_t status;

	mutex_lock(&s->mutex_bus_access);
	spi_buf[0] = 0x02 | ((reg + portnum * 0x08) << CH43X_REG_SHIFT);

	spi_buf[1] = val;

	status = spi_write(s->spi_dev, spi_buf, 2);
	if (status < 0) {
		dev_err(&s->spi_dev->dev, "Failed to ch43x_port_write Err_code %ld\n", (unsigned long)status);
	}
	mutex_unlock(&s->mutex_bus_access);

	dev_vdbg(&s->spi_dev->dev, "%s - reg:0x%2x, data:0x%2x\n", __func__, reg + portnum * 0x08, spi_buf[1]);
}

// mask: bit to operate, val: 0 to clear, mask to set
static void ch43x_port_update(struct uart_port *port, u8 reg, u8 mask, u8 val)
{
	unsigned int tmp;

	tmp = ch43x_port_read(port, reg);
	tmp &= ~mask;
	tmp |= val & mask;
	ch43x_port_write(port, reg, tmp);
}

// mask: bit to operate, val: 0 to clear, mask to set
static void ch43x_port_update_specify(struct uart_port *port, u8 portnum, u8 reg, u8 mask, u8 val)
{
	unsigned int tmp;

	tmp = ch43x_port_read_specify(port, portnum, reg);
	tmp &= ~mask;
	tmp |= val & mask;
	ch43x_port_write_spefify(port, portnum, reg, tmp);
}

void ch43x_raw_write(struct uart_port *port, const void *reg, unsigned char *buf, int len)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	ssize_t status;
	struct spi_message m;
	struct spi_transfer t[2] = {
		{
			.tx_buf = reg,
			.len = 1,
		},
		{
			.tx_buf = buf,
			.len = len,
		},
	};
	int i;

	mutex_lock(&s->mutex_bus_access);
	spi_message_init(&m);
	spi_message_add_tail(&t[0], &m);
	spi_message_add_tail(&t[1], &m);
	status = spi_sync(s->spi_dev, &m);
	if (status < 0) {
		dev_err(&s->spi_dev->dev, "Failed to ch43x_raw_write Err_code %ld\n", (unsigned long)status);
	}
	dev_vdbg(&s->spi_dev->dev, "%s - reg:0x%2x\n", __func__, *(u8 *)reg);
	for (i = 0; i < len; i++)
		dev_vdbg(&s->spi_dev->dev, "\tbuf[%d]:0x%2x\n", i, buf[i]);

	mutex_unlock(&s->mutex_bus_access);
}

void ch43x_raw_read(struct uart_port *port, unsigned char *buf, int len)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	unsigned char cmd;
	ssize_t status;

	mutex_lock(&s->mutex_bus_access);
	cmd = 0xFD & ((CH43X_RHR_REG + port->line * 0x08) << CH43X_REG_SHIFT);
	status = spi_write_then_read(s->spi_dev, &cmd, 1, buf, len);
	mutex_unlock(&s->mutex_bus_access);
	if (status < 0) {
		dev_err(&s->spi_dev->dev, "Failed to ch43x_raw_read Err_code %ld\n", (unsigned long)status);
	}
	dev_vdbg(&s->spi_dev->dev, "%s - reg:0x%2x, len:0x%d\n", __func__, CH43X_RHR_REG + port->line * 0x08, len);
}
#endif

static void ch43x_power(struct uart_port *port, int on)
{
	ch43x_port_update_specify(port, 0, CH43X_IER_REG, CH43X_IER_SLEEP_BIT, on ? 0 : CH43X_IER_SLEEP_BIT);
}

static struct ch43x_devtype ch43x_devtype = {
	.name = "CH43X",
	.nr_uart = 2,
};

static int ch43x_set_baud(struct uart_port *port, int baud)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	u8 lcr;
	unsigned long clk = port->uartclk;
	unsigned long div;

	dev_dbg(&s->spi_dev->dev, "%s - %d\n", __func__, baud);

    /* when use clock multipication */
    div = clk / 16 / baud;

	lcr = ch43x_port_read(port, CH43X_LCR_REG);

    /* Open the LCR divisors for configuration */
    ch43x_port_write(port, CH43X_LCR_REG, CH43X_LCR_CONF_MODE_A);

	/* Write the new divisor */
	ch43x_port_write(port, CH43X_DLH_REG, div / 256);
	ch43x_port_write(port, CH43X_DLL_REG, div % 256);

	/* Put LCR back to the normal mode */
	ch43x_port_write(port, CH43X_LCR_REG, lcr);

	return DIV_ROUND_CLOSEST(clk / 16, div);
}

static int ch43x_dump_register(struct uart_port *port)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	u8 lcr;
	u8 i, reg;

	lcr = ch43x_port_read(port, CH43X_LCR_REG);
	ch43x_port_write(port, CH43X_LCR_REG, CH43X_LCR_CONF_MODE_A);
	dev_vdbg(&s->spi_dev->dev, "******Dump register at LCR=DLAB\n");
	for (i = 0; i < 1; i++) {
		reg = ch43x_port_read(port, i);
		dev_vdbg(&s->spi_dev->dev, "Reg[0x%02x] = 0x%02x\n", i, reg);
	}

	ch43x_port_update(port, CH43X_LCR_REG, CH43X_LCR_CONF_MODE_A, 0);
	dev_vdbg(&s->spi_dev->dev, "******Dump register at LCR=Normal\n");
	for (i = 0; i < 16; i++) {
		reg = ch43x_port_read(port, i);
		dev_vdbg(&s->spi_dev->dev, "Reg[0x%02x] = 0x%02x\n", i, reg);
	}

	/* Put LCR back to the normal mode */
	ch43x_port_write(port, CH43X_LCR_REG, lcr);

	return 0;
}

static int ch43x_spi_test(struct uart_port *port)
{
	u8 val;
	struct ch43x_port *s = dev_get_drvdata(port->dev);

	dev_vdbg(&s->spi_dev->dev, "******Uart %d SPR Test Start******\n", port->line);

	ch43x_port_read(port, CH43X_IIR_REG);
	ch43x_port_read(port, CH43X_LSR_REG);

	ch43x_port_write(port, CH43X_SPR_REG, 0x55);
	val = ch43x_port_read(port, CH43X_SPR_REG);
	if (val != 0x55) {
		dev_err(&s->spi_dev->dev, "UART %d SPR Test Failed.\n", port->line);
		return -1;
	}

	ch43x_port_write(port, CH43X_SPR_REG, 0xAA);
	val = ch43x_port_read(port, CH43X_SPR_REG);
	if (val != 0xAA) {
		dev_err(&s->spi_dev->dev, "UART %d SPR Test Failed.\n", port->line);
		return -1;
	}

	dev_vdbg(&s->spi_dev->dev, "******Uart %d SPR Test End******\n", port->line);
	return 0;
}

static void ch43x_handle_rx(struct uart_port *port, unsigned int iir)
{
    struct ch43x_port *s = dev_get_drvdata(port->dev);
    unsigned int lsr = 0, ch, flag, bytes_read = 0;
    bool read_lsr = (iir == CH43X_IIR_RLSE_SRC) ? true : false;

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	/* Only read lsr if there are possible errors in FIFO */
	if (read_lsr) {
		lsr = ch43x_port_read(port, CH43X_LSR_REG);
		/* No errors left in FIFO */
		if (!(lsr & CH43X_LSR_FIFOE_BIT))
			read_lsr = false;
	} else
		lsr = 0;

	/* At lest one error left in FIFO */
	if (read_lsr) {
		ch = ch43x_port_read(port, CH43X_RHR_REG);
		bytes_read = 1;

		goto ch_handler;
	} else {
		while (((lsr = ch43x_port_read(port, CH43X_LSR_REG)) & CH43X_LSR_DR_BIT) == 0)
			;

		do {
			if (likely(lsr & CH43X_LSR_DR_BIT)) {
				ch = ch43x_port_read(port, CH43X_RHR_REG);
				bytes_read++;
			} else
				break;
ch_handler:
			flag = TTY_NORMAL;
			port->icount.rx++;

			if (unlikely(lsr & CH43X_LSR_BRK_ERROR_MASK)) {
				if (lsr & CH43X_LSR_BI_BIT) {
					lsr &= ~(CH43X_LSR_FE_BIT | CH43X_LSR_PE_BIT);
					port->icount.brk++;
					if (uart_handle_break(port))
						goto ignore_char;
				} else if (lsr & CH43X_LSR_PE_BIT)
					port->icount.parity++;
				else if (lsr & CH43X_LSR_FE_BIT)
					port->icount.frame++;
				else if (lsr & CH43X_LSR_OE_BIT)
					port->icount.overrun++;

				lsr &= port->read_status_mask;
				if (lsr & CH43X_LSR_BI_BIT)
					flag = TTY_BREAK;
				else if (lsr & CH43X_LSR_PE_BIT)
					flag = TTY_PARITY;
				else if (lsr & CH43X_LSR_FE_BIT)
					flag = TTY_FRAME;

				if (lsr & CH43X_LSR_OE_BIT)
					dev_err(&s->spi_dev->dev, "%s - overrun detect\n", __func__);
			}

			if (uart_handle_sysrq_char(port, ch)) {
				goto ignore_char;
			}
			uart_insert_char(port, lsr, CH43X_LSR_OE_BIT, ch, flag);
ignore_char:
			lsr = ch43x_port_read(port, CH43X_LSR_REG);
		} while ((lsr & CH43X_LSR_DR_BIT));
	}
	dev_vdbg(&s->spi_dev->dev, "%s-bytes_read:%d\n", __func__, bytes_read);
	tty_flip_buffer_push(&port->state->port);
}

static void ch43x_handle_tx(struct uart_port *port)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	struct circ_buf *xmit = &port->state->xmit;
	unsigned int txlen, to_send, i;
	unsigned char thr_reg;

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	/* xon/xoff char */
	if (unlikely(port->x_char)) {
		ch43x_port_write(port, CH43X_THR_REG, port->x_char);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
		dev_vdbg(&s->spi_dev->dev, "ch43x_handle_tx stopped\n");
		// add on 20200608
		ch43x_port_update(port, CH43X_IER_REG, CH43X_IER_THRI_BIT, 0);
		return;
	}

	/* Get length of data pending in circular buffer */
	to_send = uart_circ_chars_pending(xmit);

	if (likely(to_send)) {
		/* Limit to size of TX FIFO */
		txlen = CH43X_FIFO_SIZE;
		to_send = (to_send > txlen) ? txlen : to_send;

		/* Add data to send */
		port->icount.tx += to_send;

		/* Convert to linear buffer */
		for (i = 0; i < to_send; ++i) {
			s->buf[i] = xmit->buf[xmit->tail];
			xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		}
		dev_vdbg(&s->spi_dev->dev, "ch43x_handle_tx %d bytes\n", to_send);
		thr_reg = 0x02 | ((CH43X_THR_REG + port->line * 0x08) << CH43X_REG_SHIFT);
		ch43x_raw_write(port, &thr_reg, s->buf, to_send);
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

static void ch43x_port_irq(struct ch43x_port *s, int portno)
{
	struct uart_port *port = &s->p[portno].port;

	do {
		unsigned int iir, msr;
		unsigned char lsr;
		lsr = ch43x_port_read(port, CH43X_LSR_REG);
		if (lsr & 0x02) {
			dev_err(port->dev, "Rx Overrun portno = %d, lsr = 0x%2x\n", portno, lsr);
		}

		// add on 20200608
		/*
		msr = ch43x_port_read(port, CH43X_MSR_REG);
		s->p[portno].msr_reg = msr; 
		dev_vdbg(&s->spi_dev->dev, "uart_update_modem = 0x%02x\n", msr);
		uart_handle_cts_change(port, !!(msr & CH43X_MSR_CTS_BIT));
		*/

		iir = ch43x_port_read(port, CH43X_IIR_REG);
		if (iir & CH43X_IIR_NO_INT_BIT) {
			dev_vdbg(&s->spi_dev->dev, "%s no int, quit\n", __func__);
			break;
		}
		iir &= CH43X_IIR_ID_MASK;
		switch (iir) {
		case CH43X_IIR_RDI_SRC:
		case CH43X_IIR_RLSE_SRC:
		case CH43X_IIR_RTOI_SRC:
			ch43x_handle_rx(port, iir);
			break;
		case CH43X_IIR_MSI_SRC:
			msr = ch43x_port_read(port, CH43X_MSR_REG);
			s->p[portno].msr_reg = msr;
			dev_vdbg(&s->spi_dev->dev, "uart_handle_modem_change = 0x%02x\n", msr);
			break;
		case CH43X_IIR_THRI_SRC:
			mutex_lock(&s->mutex);
			ch43x_handle_tx(port);
			mutex_unlock(&s->mutex);
			break;
		default:
			dev_err(port->dev, "Port %i: Unexpected interrupt: %x", port->line, iir);
			break;
		}
	} while (1);
}

static irqreturn_t ch43x_ist_top(int irq, void *dev_id)
{
	return IRQ_WAKE_THREAD;
}

static irqreturn_t ch43x_ist(int irq, void *dev_id)
{
	struct ch43x_port *s = (struct ch43x_port *)dev_id;
	int i;

	dev_dbg(&s->spi_dev->dev, "ch43x_ist interrupt enter...\n");

	for (i = 0; i < s->uart.nr; ++i)
		ch43x_port_irq(s, i);

	dev_dbg(&s->spi_dev->dev, "%s end\n", __func__);

	return IRQ_HANDLED;
}

static void ch43x_wq_proc(struct work_struct *ws)
{
	struct ch43x_one *one = to_ch43x_one(ws, tx_work);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	mutex_lock(&s->mutex);
	ch43x_port_update(&one->port, CH43X_IER_REG, CH43X_IER_THRI_BIT, CH43X_IER_THRI_BIT);
	//	ch43x_handle_tx(&one->port);
	mutex_unlock(&s->mutex);
}

static void ch43x_stop_tx(struct uart_port *port)
{
	struct ch43x_one *one = to_ch43x_one(port, port);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	schedule_work(&one->stop_tx_work);
}

static void ch43x_stop_rx(struct uart_port *port)
{
	struct ch43x_one *one = to_ch43x_one(port, port);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	schedule_work(&one->stop_rx_work);
}

static void ch43x_start_tx(struct uart_port *port)
{
	struct ch43x_one *one = to_ch43x_one(port, port);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);

	/* handle rs485 */
	if ((one->rs485.flags & SER_RS485_ENABLED) && (one->rs485.delay_rts_before_send > 0)) {
		mdelay(one->rs485.delay_rts_before_send);
	}
	if (!work_pending(&one->tx_work)) {
		dev_dbg(&s->spi_dev->dev, "%s schedule\n", __func__);
		schedule_work(&one->tx_work);
	}
}

static void ch43x_stop_rx_work_proc(struct work_struct *ws)
{
	struct ch43x_one *one = to_ch43x_one(ws, stop_rx_work);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	mutex_lock(&s->mutex);
	one->port.read_status_mask &= ~CH43X_LSR_DR_BIT;
	ch43x_port_update(&one->port, CH43X_IER_REG, CH43X_IER_RDI_BIT, 0);
	ch43x_port_update(&one->port, CH43X_IER_REG, CH43X_IER_RLSI_BIT, 0);
	mutex_unlock(&s->mutex);
}

static void ch43x_stop_tx_work_proc(struct work_struct *ws)
{
	struct ch43x_one *one = to_ch43x_one(ws, stop_tx_work);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);
	struct circ_buf *xmit = &one->port.state->xmit;

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	mutex_lock(&s->mutex);
	/* handle rs485 */
	if (one->rs485.flags & SER_RS485_ENABLED) {
		/* do nothing if current tx not yet completed */
		int lsr = ch43x_port_read(&one->port, CH43X_LSR_REG);
		if (!(lsr & CH43X_LSR_TEMT_BIT)) {
			mutex_unlock(&s->mutex);
			return;
		}
		if (uart_circ_empty(xmit) && (one->rs485.delay_rts_after_send > 0))
			mdelay(one->rs485.delay_rts_after_send);
	}

	ch43x_port_update(&one->port, CH43X_IER_REG, CH43X_IER_THRI_BIT, 0);
	mutex_unlock(&s->mutex);
}

static unsigned int ch43x_tx_empty(struct uart_port *port)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	unsigned int lsr;
	unsigned int result;

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	lsr = ch43x_port_read(port, CH43X_LSR_REG);
	result = (lsr & CH43X_LSR_THRE_BIT) ? TIOCSER_TEMT : 0;

	return result;
}

static unsigned int ch43x_get_mctrl(struct uart_port *port)
{
	unsigned int status, ret;
	struct ch43x_port *s = dev_get_drvdata(port->dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	status = s->p[port->line].msr_reg;
	ret = 0;
	if (status & UART_MSR_DCD)
		ret |= TIOCM_CAR;
	if (status & UART_MSR_RI)
		ret |= TIOCM_RNG;
	if (status & UART_MSR_DSR)
		ret |= TIOCM_DSR;
	if (status & UART_MSR_CTS)
		ret |= TIOCM_CTS;

	return ret;
}

static void ch43x_md_proc(struct work_struct *ws)
{
	struct ch43x_one *one = to_ch43x_one(ws, md_work);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);
	unsigned int mctrl = one->port.mctrl;
	unsigned char mcr = 0;

	if (mctrl & TIOCM_RTS) {
		mcr |= UART_MCR_RTS;
	}
	if (mctrl & TIOCM_DTR) {
		mcr |= UART_MCR_DTR;
	}
	if (mctrl & TIOCM_OUT1) {
		mcr |= UART_MCR_OUT1;
	}
	if (mctrl & TIOCM_OUT2) {
		mcr |= UART_MCR_OUT2;
	}
	if (mctrl & TIOCM_LOOP) {
		mcr |= UART_MCR_LOOP;
	}

	mcr |= one->mcr_force;

	dev_dbg(&s->spi_dev->dev, "%s - mcr:0x%x, force:0x%2x\n", __func__, mcr, one->mcr_force);

	ch43x_port_write(&one->port, CH43X_MCR_REG, mcr);
}

static void ch43x_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct ch43x_one *one = to_ch43x_one(port, port);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);

	dev_dbg(&s->spi_dev->dev, "%s - mctrl:0x%x\n", __func__, mctrl);
	schedule_work(&one->md_work);
}

static void ch43x_break_ctl(struct uart_port *port, int break_state)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	ch43x_port_update(port, CH43X_LCR_REG, CH43X_LCR_TXBREAK_BIT, break_state ? CH43X_LCR_TXBREAK_BIT : 0);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
static void ch43x_set_termios(struct uart_port *port, struct ktermios *termios, const struct ktermios *old)
#else
static void ch43x_set_termios(struct uart_port *port, struct ktermios *termios, struct ktermios *old)
#endif
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	struct ch43x_one *one = to_ch43x_one(port, port);
	unsigned int lcr;
	int baud;
	u8 bParityType;

    dev_dbg(&s->spi_dev->dev, "%s\n", __func__);

	/* Mask termios capabilities we don't support */
	termios->c_cflag &= ~CMSPAR;

	/* Word size */
	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lcr = CH43X_LCR_WORD_LEN_5;
		break;
	case CS6:
		lcr = CH43X_LCR_WORD_LEN_6;
		break;
	case CS7:
		lcr = CH43X_LCR_WORD_LEN_7;
		break;
	case CS8:
		lcr = CH43X_LCR_WORD_LEN_8;
		break;
	default:
		lcr = CH43X_LCR_WORD_LEN_8;
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= CS8;
		break;
	}

	bParityType = termios->c_cflag & PARENB ?
			      (termios->c_cflag & PARODD ? 1 : 2) + (termios->c_cflag & CMSPAR ? 2 : 0) :
			      0;
	lcr |= CH43X_LCR_PARITY_BIT;
	switch (bParityType) {
	case 0x01:
		lcr |= CH43X_LCR_ODDPARITY_BIT;
		dev_vdbg(&s->spi_dev->dev, "parity = odd\n");
		break;
	case 0x02:
		lcr |= CH43X_LCR_EVENPARITY_BIT;
		dev_vdbg(&s->spi_dev->dev, "parity = even\n");
		break;
	case 0x03:
		lcr |= CH43X_LCR_MARKPARITY_BIT;
		dev_vdbg(&s->spi_dev->dev, "parity = mark\n");
		break;
	case 0x04:
		lcr |= CH43X_LCR_SPACEPARITY_BIT;
		dev_vdbg(&s->spi_dev->dev, "parity = space\n");
		break;
	default:
		lcr &= ~CH43X_LCR_PARITY_BIT;
		dev_vdbg(&s->spi_dev->dev, "parity = none\n");
		break;
	}

	/* Stop bits */
	if (termios->c_cflag & CSTOPB)
		lcr |= CH43X_LCR_STOPLEN_BIT; /* 2 stops */

	/* Set read status mask */
	port->read_status_mask = CH43X_LSR_OE_BIT;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= CH43X_LSR_PE_BIT | CH43X_LSR_FE_BIT;
	if (termios->c_iflag & (BRKINT | PARMRK))
		port->read_status_mask |= CH43X_LSR_BI_BIT;

	/* Set status ignore mask */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNBRK)
		port->ignore_status_mask |= CH43X_LSR_BI_BIT;
	if (!(termios->c_cflag & CREAD))
		port->ignore_status_mask |= CH43X_LSR_BRK_ERROR_MASK;

	/* Update LCR register */
	ch43x_port_write(port, CH43X_LCR_REG, lcr);

	/* Configure flow control */
	if (termios->c_cflag & CRTSCTS) {
		dev_vdbg(&s->spi_dev->dev, "ch43x_set_termios enable rts/cts\n");
		ch43x_port_update(port, CH43X_MCR_REG, CH43X_MCR_AFE | CH43X_MCR_RTS_BIT,
				  CH43X_MCR_AFE | CH43X_MCR_RTS_BIT);
		one->mcr_force |= CH43X_MCR_AFE | CH43X_MCR_RTS_BIT;
		// add on 20200608 suppose cts status is always valid here
		uart_handle_cts_change(port, 1);
	} else {
		dev_vdbg(&s->spi_dev->dev, "ch43x_set_termios disable rts/cts\n");
		ch43x_port_update(port, CH43X_MCR_REG, CH43X_MCR_AFE, 0);
		one->mcr_force &= ~(CH43X_MCR_AFE | CH43X_MCR_RTS_BIT);
	}

	/* Get baud rate generator configuration */
	baud = uart_get_baud_rate(port, termios, old, port->uartclk / 16 / 0xffff, port->uartclk / 16 * 24);
	/* Setup baudrate generator */
	baud = ch43x_set_baud(port, baud);
	/* Update timeout according to new baud rate */
	uart_update_timeout(port, termios->c_cflag, baud);
	//ch43x_dump_register(port);
}

static void ch43x_config_rs485(struct uart_port *port, struct serial_rs485 *rs485)
{
	struct ch43x_one *one = to_ch43x_one(port, port);
	struct ch43x_port *s = dev_get_drvdata(one->port.dev);

	one->rs485 = *rs485;
	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	if (one->rs485.flags & SER_RS485_ENABLED) {
	} else {
	}
}

static int ch43x_ioctl(struct uart_port *port, unsigned int cmd, unsigned long arg)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	struct serial_rs485 rs485;

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);

	switch (cmd) {
	case IOCTL_CMD_SRS485:
		if (copy_from_user(&rs485, (void __user *)arg, sizeof(rs485)))
			return -EFAULT;

		ch43x_config_rs485(port, &rs485);
		return 0;
	case IOCTL_CMD_GRS485:
		if (copy_to_user((void __user *)arg, &(to_ch43x_one(port, port)->rs485), sizeof(rs485)))
			return -EFAULT;
		return 0;
	default:
		break;
	}

	return -ENOIOCTLCMD;
}

static int ch43x_startup(struct uart_port *port)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	unsigned int val;
	struct ch43x_one *one = to_ch43x_one(port, port);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);

	ch43x_power(port, 1);
	/* Reset FIFOs*/
	val = CH43X_FCR_RXRESET_BIT | CH43X_FCR_TXRESET_BIT;
	ch43x_port_write(port, CH43X_FCR_REG, val);
	udelay(5);
	/* Enable FIFOs and configure interrupt & flow control levels to 8 */
	ch43x_port_write(port, CH43X_FCR_REG, CH43X_FCR_RXLVLH_BIT | CH43X_FCR_FIFO_BIT);

	/* Now, initialize the UART */
	ch43x_port_write(port, CH43X_LCR_REG, CH43X_LCR_WORD_LEN_8);

    /* Enable RX, CTS change interrupts */
    val = ch43x_port_read(port, CH43X_IER_REG);
    val |= CH43X_IER_RDI_BIT | CH43X_IER_RLSI_BIT | CH43X_IER_MSI_BIT;
    ch43x_port_write(port, CH43X_IER_REG, val);

	/* Enable Uart interrupts */
	ch43x_port_write(port, CH43X_MCR_REG, CH43X_MCR_OUT2);
	one->mcr_force = CH43X_MCR_OUT2;

	return 0;
}

static void ch43x_shutdown(struct uart_port *port)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	struct ch43x_one *one = to_ch43x_one(port, port);

    dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
    dev_vdbg(&s->spi_dev->dev, "MCR:0x%x\n", ch43x_port_read(port, CH43X_MCR_REG));
    dev_vdbg(&s->spi_dev->dev, "LSR:0x%x\n", ch43x_port_read(port, CH43X_LSR_REG));
    dev_vdbg(&s->spi_dev->dev, "IIR:0x%x\n", ch43x_port_read(port, CH43X_IIR_REG));

    /* Disable uart0 interrupts */
    if (port->line == 0)
        ch43x_port_write(port, CH43X_IER_REG, 0);
    ch43x_port_write(port, CH43X_MCR_REG, 0);

    one->mcr_force = 0;
    ch43x_power(port, 0);
}

static const char *ch43x_type(struct uart_port *port)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);
	return (port->type == PORT_SC16IS7XX) ? s->devtype->name : NULL;
}

static int ch43x_request_port(struct uart_port *port)
{
	/* Do nothing */
	return 0;
}

static void ch43x_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_SC16IS7XX;
}

static int ch43x_verify_port(struct uart_port *port, struct serial_struct *s)
{
	if ((s->type != PORT_UNKNOWN) && (s->type != PORT_SC16IS7XX))
		return -EINVAL;
	if (s->irq != port->irq)
		return -EINVAL;

	return 0;
}

static void ch43x_pm(struct uart_port *port, unsigned int state, unsigned int oldstate)
{
	struct ch43x_port *s = dev_get_drvdata(port->dev);

	dev_dbg(&s->spi_dev->dev, "%s\n", __func__);
	ch43x_power(port, (state == UART_PM_STATE_ON) ? 1 : 0);
}

static void ch43x_null_void(struct uart_port *port)
{
	/* Do nothing */
}
static void ch43x_enable_ms(struct uart_port *port)
{
	/* Do nothing */
}

static const struct uart_ops ch43x_ops = {
	.tx_empty = ch43x_tx_empty,
	.set_mctrl = ch43x_set_mctrl,
	.get_mctrl = ch43x_get_mctrl,
	.stop_tx = ch43x_stop_tx,
	.start_tx = ch43x_start_tx,
	.stop_rx = ch43x_stop_rx,
	.break_ctl = ch43x_break_ctl,
	.startup = ch43x_startup,
	.shutdown = ch43x_shutdown,
	.set_termios = ch43x_set_termios,
	.type = ch43x_type,
	.request_port = ch43x_request_port,
	.release_port = ch43x_null_void,
	.config_port = ch43x_config_port,
	.verify_port = ch43x_verify_port,
	.ioctl = ch43x_ioctl,
	.enable_ms = ch43x_enable_ms,
	.pm = ch43x_pm,
};

static ssize_t reg_dump_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct uart_port *port;
    int i;

    dev_info(dev, "reg_dump_show");

    for (i = 0; i < 2; i++) {
        port = &g_ch43x_port->p[i].port;
        ch43x_dump_register(port);
    }

    return 0;
}

static ssize_t reg_dump_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    return count;
}

static DEVICE_ATTR(reg_dump, S_IRUGO | S_IWUSR, reg_dump_show, reg_dump_store);

static struct attribute *ch432_attributes[] = {&dev_attr_reg_dump.attr, NULL};

static struct attribute_group ch432_attribute_group = {.attrs = ch432_attributes};

int ch432_create_sysfs(struct spi_device *spi)
{
    int err;

    err = sysfs_create_group(&spi->dev.kobj, &ch432_attribute_group);
    if (err != 0) {
        dev_err(&spi->dev, "sysfs_create_group() failed!!");
        sysfs_remove_group(&spi->dev.kobj, &ch432_attribute_group);
        return -EIO;
    }

    err = sysfs_create_link(NULL, &spi->dev.kobj, "ch432");
    if (err < 0) {
        dev_err(&spi->dev, "Failed to create link!");
        return -EIO;
    }

    dev_info(&spi->dev, "sysfs_create_group() succeeded!!");

    return err;
}

static int ch43x_probe(struct spi_device *spi, struct ch43x_devtype *devtype, int irq, unsigned long flags)
{
	unsigned long freq;
	int i, ret;
	struct ch43x_port *s;
	struct device *dev = &spi->dev;

	/* Alloc port structure */
	s = devm_kzalloc(dev, sizeof(*s) + sizeof(struct ch43x_one) * devtype->nr_uart, GFP_KERNEL);
	if (!s) {
		dev_err(dev, "Error allocating port structure\n");
		return -ENOMEM;
	}

    /* 22.1184Mhz Crystal by default, uart clock is processed to double frequency, refer CH432DS1.PDF chapter 5.2 */
    freq = CRYSTAL_FREQ * 2;
	s->devtype = devtype;
	dev_set_drvdata(dev, s);
	s->spi_dev = spi;

	/* Register UART driver */
	s->uart.owner = THIS_MODULE;
	s->uart.dev_name = "ttyWCH";
	s->uart.nr = devtype->nr_uart;
	ret = uart_register_driver(&s->uart);
	if (ret) {
		dev_err(dev, "Registering UART driver failed\n");
		goto out_clk;
	}

	mutex_init(&s->mutex);
	mutex_init(&s->mutex_bus_access);
	for (i = 0; i < devtype->nr_uart; ++i) {
		/* Initialize port data */
		s->p[i].port.line = i;
		s->p[i].port.dev = dev;
		s->p[i].port.irq = irq;
		s->p[i].port.type = PORT_SC16IS7XX;
		s->p[i].port.fifosize = CH43X_FIFO_SIZE;
		s->p[i].port.flags = UPF_FIXED_TYPE | UPF_LOW_LATENCY;
		s->p[i].port.iotype = UPIO_PORT;
		s->p[i].port.uartclk = freq;
		s->p[i].port.ops = &ch43x_ops;
		/* Disable all interrupts */
		ch43x_port_write(&s->p[i].port, CH43X_IER_REG, 0);
		/* Disable uart interrupts */
		ch43x_port_write(&s->p[i].port, CH43X_MCR_REG, 0);

		s->p[i].msr_reg = ch43x_port_read(&s->p[i].port, CH43X_MSR_REG);

		/* Initialize queue for start TX */
		INIT_WORK(&s->p[i].tx_work, ch43x_wq_proc);
		/* Initialize queue for changing mode */
		INIT_WORK(&s->p[i].md_work, ch43x_md_proc);

		INIT_WORK(&s->p[i].stop_rx_work, ch43x_stop_rx_work_proc);
		INIT_WORK(&s->p[i].stop_tx_work, ch43x_stop_tx_work_proc);

		/* Register port */
		uart_add_one_port(&s->uart, &s->p[i].port);
		ret = ch43x_spi_test(&s->p[i].port);
		if (ret)
			goto out;
		/* Go to suspend mode */
		ch43x_power(&s->p[i].port, 0);
	}

    ch43x_port_update_specify(&s->p[1].port, 1, CH43X_IER_REG, CH43X_IER_CK2X_BIT, CH43X_IER_CK2X_BIT);
	ret = devm_request_threaded_irq(dev, irq, ch43x_ist_top, ch43x_ist,
					//					IRQF_ONESHOT | flags, dev_name(dev), s);
					flags, dev_name(dev), s);

	dev_dbg(dev, "%s - devm_request_threaded_irq =%d result:%d\n", __func__, irq, ret);
    g_ch43x_port = s;

	if (!ret)
		return 0;

out:
	mutex_destroy(&s->mutex);

	uart_unregister_driver(&s->uart);

out_clk:
	if (!IS_ERR(s->clk))
		/*clk_disable_unprepare(s->clk)*/;

	return ret;
}

static int ch43x_remove(struct device *dev)
{
	struct ch43x_port *s = dev_get_drvdata(dev);
	int i;

	dev_dbg(dev, "%s\n", __func__);

	for (i = 0; i < s->uart.nr; i++) {
		cancel_work_sync(&s->p[i].tx_work);
		cancel_work_sync(&s->p[i].md_work);
		cancel_work_sync(&s->p[i].stop_rx_work);
		cancel_work_sync(&s->p[i].stop_tx_work);
		uart_remove_one_port(&s->uart, &s->p[i].port);
		ch43x_power(&s->p[i].port, 0);
	}

	mutex_destroy(&s->mutex);
	mutex_destroy(&s->mutex_bus_access);
	uart_unregister_driver(&s->uart);
	if (!IS_ERR(s->clk))
		/*clk_disable_unprepare(s->clk)*/;

	return 0;
}

static const struct of_device_id __maybe_unused ch43x_dt_ids[] = {
	{
		.compatible = "wch,ch43x",
		.data = &ch43x_devtype,
	},
	{},
};
MODULE_DEVICE_TABLE(of, ch43x_dt_ids);

#ifdef USE_SPI_MODE
static int ch43x_spi_probe(struct spi_device *spi)
{
	struct ch43x_devtype *devtype = &ch43x_devtype;
	unsigned long flags = IRQF_TRIGGER_FALLING;
	int ret;
	u32 save;

	dev_dbg(&spi->dev, "gpio_to_irq:%d, spi->irq:%d\n", gpio_to_irq(GPIO_NUMBER), spi->irq);
	save = spi->mode;
	spi->mode |= SPI_MODE_3;

    spi->max_speed_hz = 20000000;
	if (spi_setup(spi) < 0) {
		spi->mode = save;
	} else {
		dev_dbg(&spi->dev, "change to SPI MODE 3!\n");
	}

/* if your platform supports acquire irq number from dts */
#ifdef USE_IRQ_FROM_DTS
	ret = ch43x_probe(spi, devtype, spi->irq, flags);
#else
	ret = devm_gpio_request(&spi->dev, GPIO_NUMBER, "gpioint");
	if (ret) {
		dev_err(&spi->dev, "gpio_request\n");
	}
	ret = gpio_direction_input(GPIO_NUMBER);
	if (ret) {
		dev_err(&spi->dev, "gpio_direction_input\n");
	}
	irq_set_irq_type(gpio_to_irq(GPIO_NUMBER), flags);
	ret = ch43x_probe(spi, devtype, gpio_to_irq(GPIO_NUMBER), flags);
#endif

    // ch432_create_sysfs(spi);

    return ret;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))
static void ch43x_spi_remove(struct spi_device *spi)
#else
static int ch43x_spi_remove(struct spi_device *spi)
#endif
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))
    ch43x_remove(&spi->dev);
#else
    return ch43x_remove(&spi->dev);
#endif
}

static struct spi_driver ch43x_spi_uart_driver = {
   .driver = {
	   .name   = CH43X_NAME_SPI,
	   .bus    = &spi_bus_type,
	   .owner  = THIS_MODULE,
	   .of_match_table = of_match_ptr(ch43x_dt_ids),
   },
   .probe          = ch43x_spi_probe,
   .remove         = ch43x_spi_remove,
};

MODULE_ALIAS("spi:ch43x");
#endif

#ifdef USE_SPI_MODE
static int __init ch43x_init(void)
{
	printk(KERN_INFO KBUILD_MODNAME ": " DRIVER_DESC "\n");
	printk(KERN_INFO KBUILD_MODNAME ": " VERSION_DESC "\n");
	return spi_register_driver(&ch43x_spi_uart_driver);
}
module_init(ch43x_init);

static void __exit ch43x_exit(void)
{
	printk(KERN_INFO KBUILD_MODNAME ": "
					"ch43x driver exit.\n");
	spi_unregister_driver(&ch43x_spi_uart_driver);
}
module_exit(ch43x_exit);
#endif

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

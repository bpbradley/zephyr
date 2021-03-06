/*
 * Copyright (c) 2017, NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_lpc_usart

/** @file
 * @brief USART driver for LPC54XXX and LPC55xxx families.
 *
 * Note:
 * - The driver is implemented for only one device, multiple instances
 * will be implemented in the future.
 */

#include <errno.h>
#include <device.h>
#include <drivers/uart.h>
#include <fsl_usart.h>
#include <fsl_clock.h>
#include <soc.h>
#include <fsl_device_registers.h>

struct mcux_flexcomm_config {
	USART_Type *base;
	uint32_t clock_source;
	uint32_t baud_rate;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	void (*irq_config_func)(struct device *dev);
#endif
};

struct mcux_flexcomm_data {
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_user_data_t callback;
	void *cb_data;
#endif
};

static int mcux_flexcomm_poll_in(struct device *dev, unsigned char *c)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t flags = USART_GetStatusFlags(config->base);
	int ret = -1;

	if (flags & kUSART_RxFifoFullFlag) {
		*c = USART_ReadByte(config->base);
		ret = 0;
	}

	return ret;
}

static void mcux_flexcomm_poll_out(struct device *dev,
					     unsigned char c)
{
	const struct mcux_flexcomm_config *config = dev->config_info;

	/* Wait until space is available in TX FIFO */
	while (!(USART_GetStatusFlags(config->base) & kUSART_TxFifoEmptyFlag)) {
	}

	USART_WriteByte(config->base, c);
}

static int mcux_flexcomm_err_check(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t flags = USART_GetStatusFlags(config->base);
	int err = 0;

	if (flags & kStatus_USART_RxRingBufferOverrun) {
		err |= UART_ERROR_OVERRUN;
	}

	if (flags & kStatus_USART_ParityError) {
		err |= UART_ERROR_PARITY;
	}

	if (flags & kStatus_USART_FramingError) {
		err |= UART_ERROR_FRAMING;
	}

	USART_ClearStatusFlags(config->base,
			       kStatus_USART_RxRingBufferOverrun |
			       kStatus_USART_ParityError |
			       kStatus_USART_FramingError);

	return err;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int mcux_flexcomm_fifo_fill(struct device *dev, const uint8_t *tx_data,
			       int len)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint8_t num_tx = 0U;

	while ((len - num_tx > 0) &&
	       (USART_GetStatusFlags(config->base)
		& kUSART_TxFifoNotFullFlag)) {

		USART_WriteByte(config->base, tx_data[num_tx++]);
	}

	return num_tx;
}

static int mcux_flexcomm_fifo_read(struct device *dev, uint8_t *rx_data,
			       const int len)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint8_t num_rx = 0U;

	while ((len - num_rx > 0) &&
	       (USART_GetStatusFlags(config->base)
		& kUSART_RxFifoNotEmptyFlag)) {

		rx_data[num_rx++] = USART_ReadByte(config->base);
	}

	return num_rx;
}

static void mcux_flexcomm_irq_tx_enable(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kUSART_TxLevelInterruptEnable;

	USART_EnableInterrupts(config->base, mask);
}

static void mcux_flexcomm_irq_tx_disable(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kUSART_TxLevelInterruptEnable;

	USART_DisableInterrupts(config->base, mask);
}

static int mcux_flexcomm_irq_tx_complete(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t flags = USART_GetStatusFlags(config->base);

	return (flags & kUSART_TxFifoEmptyFlag) != 0U;
}

static int mcux_flexcomm_irq_tx_ready(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kUSART_TxLevelInterruptEnable;

	return (USART_GetEnabledInterrupts(config->base) & mask)
		&& mcux_flexcomm_irq_tx_complete(dev);
}

static void mcux_flexcomm_irq_rx_enable(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kUSART_RxLevelInterruptEnable;

	USART_EnableInterrupts(config->base, mask);
}

static void mcux_flexcomm_irq_rx_disable(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kUSART_RxLevelInterruptEnable;

	USART_DisableInterrupts(config->base, mask);
}

static int mcux_flexcomm_irq_rx_full(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t flags = USART_GetStatusFlags(config->base);

	return (flags & kUSART_RxFifoNotEmptyFlag) != 0U;
}

static int mcux_flexcomm_irq_rx_ready(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kUSART_RxLevelInterruptEnable;

	return (USART_GetEnabledInterrupts(config->base) & mask)
		&& mcux_flexcomm_irq_rx_full(dev);
}

static void mcux_flexcomm_irq_err_enable(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kStatus_USART_NoiseError |
			kStatus_USART_FramingError |
			kStatus_USART_ParityError;

	USART_EnableInterrupts(config->base, mask);
}

static void mcux_flexcomm_irq_err_disable(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	uint32_t mask = kStatus_USART_NoiseError |
			kStatus_USART_FramingError |
			kStatus_USART_ParityError;

	USART_DisableInterrupts(config->base, mask);
}

static int mcux_flexcomm_irq_is_pending(struct device *dev)
{
	return (mcux_flexcomm_irq_tx_ready(dev)
		|| mcux_flexcomm_irq_rx_ready(dev));
}

static int mcux_flexcomm_irq_update(struct device *dev)
{
	return 1;
}

static void mcux_flexcomm_irq_callback_set(struct device *dev,
				       uart_irq_callback_user_data_t cb,
				       void *cb_data)
{
	struct mcux_flexcomm_data *data = dev->driver_data;

	data->callback = cb;
	data->cb_data = cb_data;
}

static void mcux_flexcomm_isr(void *arg)
{
	struct device *dev = arg;
	struct mcux_flexcomm_data *data = dev->driver_data;

	if (data->callback) {
		data->callback(data->cb_data);
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */


static int mcux_flexcomm_init(struct device *dev)
{
	const struct mcux_flexcomm_config *config = dev->config_info;
	usart_config_t usart_config;
	uint32_t clock_freq;

	clock_freq = CLOCK_GetFlexCommClkFreq(config->clock_source);

	USART_GetDefaultConfig(&usart_config);
	usart_config.enableTx = true;
	usart_config.enableRx = true;
	usart_config.baudRate_Bps = config->baud_rate;

	USART_Init(config->base, &usart_config, clock_freq);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	config->irq_config_func(dev);
#endif

	return 0;
}

static const struct uart_driver_api mcux_flexcomm_driver_api = {
	.poll_in = mcux_flexcomm_poll_in,
	.poll_out = mcux_flexcomm_poll_out,
	.err_check = mcux_flexcomm_err_check,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = mcux_flexcomm_fifo_fill,
	.fifo_read = mcux_flexcomm_fifo_read,
	.irq_tx_enable = mcux_flexcomm_irq_tx_enable,
	.irq_tx_disable = mcux_flexcomm_irq_tx_disable,
	.irq_tx_complete = mcux_flexcomm_irq_tx_complete,
	.irq_tx_ready = mcux_flexcomm_irq_tx_ready,
	.irq_rx_enable = mcux_flexcomm_irq_rx_enable,
	.irq_rx_disable = mcux_flexcomm_irq_rx_disable,
	.irq_rx_ready = mcux_flexcomm_irq_rx_ready,
	.irq_err_enable = mcux_flexcomm_irq_err_enable,
	.irq_err_disable = mcux_flexcomm_irq_err_disable,
	.irq_is_pending = mcux_flexcomm_irq_is_pending,
	.irq_update = mcux_flexcomm_irq_update,
	.irq_callback_set = mcux_flexcomm_irq_callback_set,
#endif
};


#ifdef CONFIG_UART_INTERRUPT_DRIVEN
#define UART_MCUX_FLEXCOMM_CONFIG_FUNC(n)				\
	static void mcux_flexcomm_config_func_##n(struct device *dev)	\
	{								\
		IRQ_CONNECT(DT_INST_IRQN(n),				\
			    DT_INST_IRQ(n, priority),			\
			    mcux_flexcomm_isr, DEVICE_GET(uart_##n), 0);\
									\
		irq_enable(DT_INST_IRQN(n));				\
	}
#define UART_MCUX_FLEXCOMM_IRQ_CFG_FUNC_INIT(n)				\
	.irq_config_func = mcux_flexcomm_config_func_##n
#define UART_MCUX_FLEXCOMM_INIT_CFG(n)					\
	UART_MCUX_FLEXCOMM_DECLARE_CFG(n,				\
				       UART_MCUX_FLEXCOMM_IRQ_CFG_FUNC_INIT(n))
#else
#define UART_MCUX_FLEXCOMM_CONFIG_FUNC(n)
#define UART_MCUX_FLEXCOMM_IRQ_CFG_FUNC_INIT
#define UART_MCUX_FLEXCOMM_INIT_CFG(n)					\
	UART_MCUX_FLEXCOMM_DECLARE_CFG(n, UART_MCUX_FLEXCOMM_IRQ_CFG_FUNC_INIT)
#endif

#define UART_MCUX_FLEXCOMM_DECLARE_CFG(n, IRQ_FUNC_INIT)		\
static const struct mcux_flexcomm_config mcux_flexcomm_##n##_config = {	\
	.base = (USART_Type *)DT_INST_REG_ADDR(n),			\
	.clock_source = 0,						\
	.baud_rate = DT_INST_PROP(n, current_speed),			\
	IRQ_FUNC_INIT							\
}

#define UART_MCUX_FLEXCOMM_INIT(n)					\
									\
	static struct mcux_flexcomm_data mcux_flexcomm_##n##_data;	\
									\
	static const struct mcux_flexcomm_config mcux_flexcomm_##n##_config;\
									\
	DEVICE_AND_API_INIT(uart_##n, DT_INST_LABEL(n),			\
			    &mcux_flexcomm_init,			\
			    &mcux_flexcomm_##n##_data,			\
			    &mcux_flexcomm_##n##_config,		\
			    PRE_KERNEL_1,				\
			    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,		\
			    &mcux_flexcomm_driver_api);			\
									\
	UART_MCUX_FLEXCOMM_CONFIG_FUNC(n)				\
									\
	UART_MCUX_FLEXCOMM_INIT_CFG(n);

DT_INST_FOREACH_STATUS_OKAY(UART_MCUX_FLEXCOMM_INIT)

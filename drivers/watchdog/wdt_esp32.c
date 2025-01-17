/*
 * Copyright (C) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT espressif_esp32_watchdog

/* Include esp-idf headers first to avoid redefining BIT() macro */
#include <soc/rtc_cntl_reg.h>
#include <soc/timer_group_reg.h>

#include <string.h>
#include <drivers/watchdog.h>
#ifndef CONFIG_SOC_ESP32C3
#include <drivers/interrupt_controller/intc_esp32.h>
#else
#include <drivers/interrupt_controller/intc_esp32c3.h>
#endif
#include <device.h>

#ifdef CONFIG_SOC_ESP32C3
#define ISR_HANDLER isr_handler_t
#else
#define ISR_HANDLER intr_handler_t
#endif

/* FIXME: This struct shall be removed from here, when esp32 timer driver got
 * implemented.
 * That's why the type name starts with `timer` not `wdt`
 */
struct timer_esp32_irq_regs_t {
	uint32_t *timer_int_ena;
	uint32_t *timer_int_clr;
};

struct wdt_esp32_regs_t {
	uint32_t config0;
	uint32_t config1;
	uint32_t config2;
	uint32_t config3;
	uint32_t config4;
	uint32_t config5;
	uint32_t feed;
	uint32_t wprotect;
};

enum wdt_mode {
	WDT_MODE_RESET = 0,
	WDT_MODE_INTERRUPT_RESET
};

struct wdt_esp32_data {
	uint32_t timeout;
	enum wdt_mode mode;
	wdt_callback_t callback;
	int irq_line;
};

struct wdt_esp32_config {
	void (*connect_irq)(void);
	const struct wdt_esp32_regs_t *base;
	const struct timer_esp32_irq_regs_t irq_regs;
	int irq_source;
};

#define DEV_BASE(dev) \
	((volatile struct wdt_esp32_regs_t  *) \
	 ((const struct wdt_esp32_config *const)(dev)->config)->base)

/* ESP32 ignores writes to any register if WDTWPROTECT doesn't contain the
 * magic value of TIMG_WDT_WKEY_VALUE.  The datasheet recommends unsealing,
 * making modifications, and sealing for every watchdog modification.
 */
static inline void wdt_esp32_seal(const struct device *dev)
{
	DEV_BASE(dev)->wprotect = 0U;

}

static inline void wdt_esp32_unseal(const struct device *dev)
{
	DEV_BASE(dev)->wprotect = TIMG_WDT_WKEY_VALUE;
}

static void wdt_esp32_enable(const struct device *dev)
{
	wdt_esp32_unseal(dev);
	DEV_BASE(dev)->config0 |= BIT(TIMG_WDT_EN_S);
	wdt_esp32_seal(dev);

}

static int wdt_esp32_disable(const struct device *dev)
{
	wdt_esp32_unseal(dev);
	DEV_BASE(dev)->config0 &= ~BIT(TIMG_WDT_EN_S);
	wdt_esp32_seal(dev);

	return 0;
}

static void adjust_timeout(const struct device *dev, uint32_t timeout)
{
	/* MWDT ticks every 12.5ns.  Set the prescaler to 40000, so the
	 * counter for each watchdog stage is decremented every 0.5ms.
	 */
	DEV_BASE(dev)->config1 = 40000U;
	DEV_BASE(dev)->config2 = timeout;
	DEV_BASE(dev)->config3 = timeout;
}

static void wdt_esp32_isr(void *arg);

static int wdt_esp32_feed(const struct device *dev, int channel_id)
{
	wdt_esp32_unseal(dev);
	DEV_BASE(dev)->feed = 0xABAD1DEA; /* Writing any value to WDTFEED will reload it. */
	wdt_esp32_seal(dev);

	return 0;
}

static void set_interrupt_enabled(const struct device *dev, bool setting)
{
	const struct wdt_esp32_config *config = dev->config;
	struct wdt_esp32_data *data = dev->data;

	*config->irq_regs.timer_int_clr |= TIMG_WDT_INT_CLR;

	if (setting) {
		*config->irq_regs.timer_int_ena |= TIMG_WDT_INT_ENA;
		irq_enable(data->irq_line);
	} else {
		*config->irq_regs.timer_int_ena &= ~TIMG_WDT_INT_ENA;
		irq_disable(data->irq_line);
	}
}

static int wdt_esp32_set_config(const struct device *dev, uint8_t options)
{
	struct wdt_esp32_data *data = dev->data;
	uint32_t v = DEV_BASE(dev)->config0;

	if (!data) {
		return -EINVAL;
	}

	/* Stages 3 and 4 are not used: disable them. */
	v |= TIMG_WDT_STG_SEL_OFF << TIMG_WDT_STG2_S;
	v |= TIMG_WDT_STG_SEL_OFF << TIMG_WDT_STG3_S;

	/* Wait for 3.2us before booting again. */
	v |= 7 << TIMG_WDT_SYS_RESET_LENGTH_S;
	v |= 7 << TIMG_WDT_CPU_RESET_LENGTH_S;

	if (data->mode == WDT_MODE_RESET) {
		/* Warm reset on timeout */
		v |= TIMG_WDT_STG_SEL_RESET_SYSTEM << TIMG_WDT_STG0_S;
		v |= TIMG_WDT_STG_SEL_OFF << TIMG_WDT_STG1_S;

		/* Disable interrupts for this mode. */
		#ifndef CONFIG_SOC_ESP32C3
		v &= ~(TIMG_WDT_LEVEL_INT_EN | TIMG_WDT_EDGE_INT_EN);
		#else
		v &= ~(TIMG_WDT_INT_ENA);
		#endif
	} else if (data->mode == WDT_MODE_INTERRUPT_RESET) {
		/* Interrupt first, and warm reset if not reloaded */
		v |= TIMG_WDT_STG_SEL_INT << TIMG_WDT_STG0_S;
		v |= TIMG_WDT_STG_SEL_RESET_SYSTEM << TIMG_WDT_STG1_S;

		/* Use level-triggered interrupts. */
		#ifndef CONFIG_SOC_ESP32C3
		v |= TIMG_WDT_LEVEL_INT_EN;
		v &= ~TIMG_WDT_EDGE_INT_EN;
		#else
		v |= TIMG_WDT_INT_ENA;
		#endif
	} else {
		return -EINVAL;
	}

	/* Enable the watchdog */
	v |= BIT(TIMG_WDT_EN_S)	
	
	wdt_esp32_unseal(dev);
	DEV_BASE(dev)->config0 = v;
	adjust_timeout(dev, data->timeout);
	set_interrupt_enabled(dev, data->mode == WDT_MODE_INTERRUPT_RESET);
	wdt_esp32_seal(dev);

	wdt_esp32_feed(dev, 0);

	return 0;
}

static int wdt_esp32_install_timeout(const struct device *dev,
				     const struct wdt_timeout_cfg *cfg)
{
	struct wdt_esp32_data *data = dev->data;

	if (cfg->flags != WDT_FLAG_RESET_SOC) {
		return -ENOTSUP;
	}

	if (cfg->window.min != 0U || cfg->window.max == 0U) {
		return -EINVAL;
	}

	data->timeout = cfg->window.max;

	data->mode = (cfg->callback == NULL) ?
		     WDT_MODE_RESET : WDT_MODE_INTERRUPT_RESET;

	data->callback = cfg->callback;

	return 0;
}

static int wdt_esp32_init(const struct device *dev)
{
	const struct wdt_esp32_config *const config = dev->config;
	struct wdt_esp32_data *data = dev->data;

#ifdef CONFIG_WDT_DISABLE_AT_BOOT
	wdt_esp32_disable(dev);
#endif

	/* For xtensa esp32 chips, this is a level 4 interrupt,
	 * which is handled by _Level4Vector,
	 * located in xtensa_vectors.S.
	 */
	data->irq_line = esp_intr_alloc(config->irq_source,
		0,
		(ISR_HANDLER)wdt_esp32_isr,
		(void *)dev,
		NULL);

	wdt_esp32_enable(dev);

	return 0;
}

static const struct wdt_driver_api wdt_api = {
	.setup = wdt_esp32_set_config,
	.disable = wdt_esp32_disable,
	.install_timeout = wdt_esp32_install_timeout,
	.feed = wdt_esp32_feed
};

#define ESP32_WDT_INIT(idx)							   \
	static struct wdt_esp32_data wdt##idx##_data;				   \
	static struct wdt_esp32_config wdt_esp32_config##idx = {		   \
		.base = (struct wdt_esp32_regs_t *) DT_INST_REG_ADDR(idx),	   \
		.irq_regs = {							   \
			.timer_int_ena = (uint32_t *)TIMG_INT_ENA_TIMERS_REG(idx), \
			.timer_int_clr = (uint32_t *)TIMG_INT_CLR_TIMERS_REG(idx), \
		},								   \
		.irq_source = DT_IRQN(DT_NODELABEL(wdt##idx)),			   \
	};									   \
										   \
	DEVICE_DT_INST_DEFINE(idx,						   \
			      wdt_esp32_init,					   \
			      NULL,						   \
			      &wdt##idx##_data,					   \
			      &wdt_esp32_config##idx,				   \
			      PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	   \
			      &wdt_api)

static void wdt_esp32_isr(void *arg)
{
	const struct device *dev = (const struct device *)arg;
	const struct wdt_esp32_config *config = dev->config;
	struct wdt_esp32_data *data = dev->data;

	if (data->callback) {
		data->callback(dev, 0);
	}

	*config->irq_regs.timer_int_clr |= TIMG_WDT_INT_CLR;
}


#if DT_NODE_HAS_STATUS(DT_NODELABEL(wdt0), okay)
ESP32_WDT_INIT(0);
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(wdt1), okay)
ESP32_WDT_INIT(1);
#endif

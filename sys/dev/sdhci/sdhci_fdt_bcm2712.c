/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023 Emmanuel Vadot <manu@freebsd.org>
 * Copyright (c) 2026 Jordan Hubbard
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * BCM2712 bootstrap attachment. The subclass organization follows the BSD
 * sdhci_fdt_xilinx driver; controller register semantics were independently
 * compared with Linux v6.12 sdhci-brcmstb.c, not copied from that driver.
 *
 * Only legacy/high-speed, 3.3 V operation up to 50 MHz is selected. UHS,
 * tuning, CQE, 1.8 V switching and full-RAM DMA qualification remain separate
 * work. The standard SDHCI core continues to own command and transfer logic.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/clk/clk.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/mmc/bridge.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>
#include <dev/phy/phy.h>
#include <dev/regulator/regulator.h>
#include <dev/sdhci/sdhci.h>
#include <dev/sdhci/sdhci_fdt.h>
#include <dev/sdhci/sdhci_bcm2712_policy.h>

#include "mmcbr_if.h"
#include "sdhci_if.h"
#include "opt_mmccam.h"

struct bcm2712_supply {
	regulator_t	reg;
	int		original_uv;
	bool		voltage_set;
	bool		enabled;
};

struct bcm2712_sdhci_softc {
	struct sdhci_fdt_softc base;
	struct resource *cfg;
	clk_t		clock;
	bool		clock_enabled;
	bool		slot_initialized;
	bool		cfg_saved;
	uint32_t	cfg_ctrl;
	uint32_t	cfg_mode;
	uint32_t	max_frequency;
	bool		cd_inverted;
	gpio_pin_t	cd_pin;
	gpio_pin_t	wp_pin;
	struct bcm2712_supply vmmc;
	struct bcm2712_supply vqmmc;
};

static int
bcm2712_supply_get(device_t dev, char *property, struct bcm2712_supply *s)
{
	int error;

	if (!OF_hasprop(ofw_bus_get_node(dev), property))
		return (0);
	error = regulator_get_by_ofw_property(dev, 0, property, &s->reg);
	if (error != 0)
		return (error);
	error = regulator_get_voltage(s->reg, &s->original_uv);
	if (error != 0 || s->original_uv <= 0)
		return (error != 0 ? error : ENXIO);
	error = regulator_check_voltage(s->reg, 3300000);
	if (error != 0)
		return (error);
	/* Also restore after a failed provider operation: do not assume a
	 * failing voltage change left the hardware untouched.
	 */
	s->voltage_set = true;
	error = regulator_set_voltage(s->reg, 3300000, 3300000);
	return (error);
}

static int
bcm2712_supply_power(struct bcm2712_supply *s, bool on)
{
	int error;

	if (s->reg == NULL || on == s->enabled)
		return (0);
	error = on ? regulator_enable(s->reg) : regulator_disable(s->reg);
	if (error == 0)
		s->enabled = on;
	return (error);
}

static void
bcm2712_supply_release(device_t dev, struct bcm2712_supply *s)
{
	int error;

	if (s->reg == NULL)
		return;
	error = bcm2712_supply_power(s, false);
	if (error != 0)
		device_printf(dev, "cannot release regulator enable: %d\n", error);
	if (s->voltage_set) {
		error = regulator_set_voltage(s->reg, s->original_uv,
		    s->original_uv);
		if (error != 0)
			device_printf(dev, "cannot restore regulator voltage: %d\n",
			    error);
	}
	regulator_release(s->reg);
	s->reg = NULL;
}

static void
bcm2712_sdhci_release(device_t dev)
{
	struct bcm2712_sdhci_softc *sc = device_get_softc(dev);
	struct sdhci_fdt_softc *base = &sc->base;
	int rid;

	if (base->intrhand != NULL) {
		bus_teardown_intr(dev, base->irq_res, base->intrhand);
		base->intrhand = NULL;
	}
	if (sc->slot_initialized) {
		sdhci_cleanup_slot(&base->slots[0]);
		sc->slot_initialized = false;
	}
	if (sc->cd_pin != NULL) {
		gpio_pin_release(sc->cd_pin);
		sc->cd_pin = NULL;
	}
	if (sc->wp_pin != NULL) {
		gpio_pin_release(sc->wp_pin);
		sc->wp_pin = NULL;
	}
	if (sc->cfg_saved) {
		bus_write_4(sc->cfg, BCM2712_CFG_CTRL, sc->cfg_ctrl);
		bus_write_4(sc->cfg, BCM2712_CFG_MAX_50MHZ, sc->cfg_mode);
		sc->cfg_saved = false;
	}
	bcm2712_supply_release(dev, &sc->vqmmc);
	bcm2712_supply_release(dev, &sc->vmmc);
	if (sc->clock_enabled) {
		clk_disable(sc->clock);
		sc->clock_enabled = false;
	}
	if (sc->clock != NULL) {
		clk_release(sc->clock);
		sc->clock = NULL;
	}
	if (base->irq_res != NULL) {
		rid = 0;
		bus_release_resource(dev, SYS_RES_IRQ, rid, base->irq_res);
		base->irq_res = NULL;
	}
	if (sc->cfg != NULL) {
		rid = 1;
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->cfg);
		sc->cfg = NULL;
	}
	if (base->mem_res[0] != NULL) {
		rid = 0;
		bus_release_resource(dev, SYS_RES_MEMORY, rid, base->mem_res[0]);
		base->mem_res[0] = NULL;
	}
}

static int
bcm2712_sdhci_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "brcm,bcm2712-sdhci"))
		return (ENXIO);
	device_set_desc(dev, "Broadcom BCM2712 SDHCI bootstrap controller");
	return (BUS_PROBE_DEFAULT);
}

static uint32_t
bcm2712_sdhci_read_4(device_t dev, struct sdhci_slot *slot, bus_size_t off)
{
	struct bcm2712_sdhci_softc *sc = device_get_softc(dev);
	uint32_t value;

	value = bus_read_4(sc->base.mem_res[0], off);
	if (off == SDHCI_CAPABILITIES)
		/* Force the core to retain the enabled provider's frequency for
		 * divisor math rather than replacing it with a firmware CAP field.
		 */
		value &= ~(SDHCI_CAN_VDD_180 | SDHCI_CAN_VDD_300 |
		    SDHCI_CLOCK_V3_BASE_MASK);
	if (off == SDHCI_CAPABILITIES2)
		value &= ~(SDHCI_CAN_SDR50 | SDHCI_CAN_SDR104 |
		    SDHCI_CAN_DDR50 | SDHCI_TUNE_SDR50 | SDHCI_CAN_MMC_HS400);
	return (value);
}

static int
bcm2712_sdhci_set_clock(device_t dev, struct sdhci_slot *slot, int clock)
{
	struct bcm2712_sdhci_softc *sc = device_get_softc(dev);

	return (clock < 0 ? 0 : bcm2712_sdhci_clock(clock, sc->max_frequency));
}

static bool
bcm2712_sdhci_present(device_t dev, struct sdhci_slot *slot)
{
	struct bcm2712_sdhci_softc *sc = device_get_softc(dev);
	bool active;

	if (sc->cd_pin == NULL)
		return (sdhci_generic_get_card_present(dev, slot));
	if (gpio_pin_is_active(sc->cd_pin, &active) != 0)
		return (false);
	return (active ^ sc->cd_inverted);
}

static int
bcm2712_sdhci_readonly(device_t dev, device_t child)
{
	struct bcm2712_sdhci_softc *sc = device_get_softc(dev);
	bool active;

	if (sc->base.wp_disabled)
		return (0);
	if (sc->wp_pin == NULL)
		return (sdhci_generic_get_ro(dev, child) ^ sc->base.wp_inverted);
	if (gpio_pin_is_active(sc->wp_pin, &active) != 0)
		return (1);
	return (active ^ sc->base.wp_inverted);
}

static int
bcm2712_sdhci_update_ios(device_t dev, device_t child)
{
	struct bcm2712_sdhci_softc *sc = device_get_softc(dev);
	struct sdhci_slot *slot = device_get_ivars(child);
	int error;
	bool vmmc_was_on, vqmmc_was_on;

	if (slot->host.ios.power_mode != power_off &&
	    slot->host.ios.vccq != vccq_330)
		return (EOPNOTSUPP);
	vmmc_was_on = sc->vmmc.enabled;
	vqmmc_was_on = sc->vqmmc.enabled;
	if (slot->host.ios.power_mode != power_off) {
		error = bcm2712_supply_power(&sc->vmmc, true);
		if (error == 0)
			error = bcm2712_supply_power(&sc->vqmmc, true);
		if (error != 0) {
			if (!vmmc_was_on)
				bcm2712_supply_power(&sc->vmmc, false);
			return (error);
		}
	}
	error = sdhci_generic_update_ios(dev, child);
	if (error != 0) {
		if (!vqmmc_was_on)
			bcm2712_supply_power(&sc->vqmmc, false);
		if (!vmmc_was_on)
			bcm2712_supply_power(&sc->vmmc, false);
		return (error);
	}
	if (slot->host.ios.power_mode != power_off)
		return (error);
	error = bcm2712_supply_power(&sc->vqmmc, false);
	if (error != 0)
		return (error);
	return (bcm2712_supply_power(&sc->vmmc, false));
}

static int
bcm2712_sdhci_switch_vccq(device_t dev, device_t child)
{
	struct sdhci_slot *slot = device_get_ivars(child);

	if (slot->host.ios.vccq != vccq_330)
		return (EOPNOTSUPP);
	return (sdhci_generic_switch_vccq(dev, child));
}

static void
bcm2712_sdhci_intr(void *arg)
{
	struct bcm2712_sdhci_softc *sc = arg;

	sdhci_generic_intr(&sc->base.slots[0]);
}

static int
bcm2712_sdhci_pin(device_t dev, char *property, gpio_pin_t *pin)
{
	int error;

	if (!OF_hasprop(ofw_bus_get_node(dev), property))
		return (0);
	error = gpio_pin_get_by_ofw_property(dev, 0, property, pin);
	if (error == 0)
		error = gpio_pin_setflags(*pin, GPIO_PIN_INPUT);
	return (error);
}

static int
bcm2712_sdhci_attach(device_t dev)
{
	struct bcm2712_sdhci_softc *sc = device_get_softc(dev);
	struct sdhci_fdt_softc *base = &sc->base;
	struct sdhci_slot *slot = &base->slots[0];
	phandle_t node = ofw_bus_get_node(dev);
	uint64_t frequency;
	uint32_t width;
	int error, rid, host_index, cfg_index, clock_index;
	const char *stage = "host/cfg resources";

	memset(sc, 0, sizeof(*sc));
	base->dev = dev;
#ifdef MMCCAM
	device_printf(dev, "MMCCAM power/IOS lifecycle is not supported by this bootstrap profile\n");
	return (EOPNOTSUPP);
#endif
	if (ofw_bus_find_string_index(node, "reg-names", "host", &host_index) != 0 ||
	    ofw_bus_find_string_index(node, "reg-names", "cfg", &cfg_index) != 0 ||
	    host_index != 0 || cfg_index != 1)
		return (EINVAL);
	if (OF_hasprop(node, "mmc-pwrseq")) {
		device_printf(dev, "mmc-pwrseq requires a separate power-sequence implementation\n");
		return (EOPNOTSUPP);
	}
	if (ofw_bus_find_string_index(node, "clock-names", "sdio_freq",
	    &clock_index) == 0) {
		device_printf(dev, "enhanced sdio_freq clock requires separate rate-change support\n");
		return (EOPNOTSUPP);
	}
	width = 1;
	if (OF_hasprop(node, "bus-width") &&
	    OF_getencprop(node, "bus-width", &width, sizeof(width)) != sizeof(width))
		return (EINVAL);
	if (width != 1 && width != 4 && width != 8)
		return (EINVAL);
	sc->max_frequency = BCM2712_BOOTSTRAP_MAX_HZ;
	if (OF_hasprop(node, "max-frequency") &&
	    (OF_getencprop(node, "max-frequency", &sc->max_frequency,
	    sizeof(sc->max_frequency)) != sizeof(sc->max_frequency) ||
	    sc->max_frequency == 0))
		return (EINVAL);
	sc->max_frequency = MIN(sc->max_frequency, BCM2712_BOOTSTRAP_MAX_HZ);
	base->wp_disabled = OF_hasprop(node, "disable-wp");
	base->wp_inverted = OF_hasprop(node, "wp-inverted");
	sc->cd_inverted = OF_hasprop(node, "cd-inverted");
	base->no_18v = true;
	rid = 0;
	base->mem_res[0] = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	rid = 1;
	sc->cfg = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (base->mem_res[0] == NULL || sc->cfg == NULL ||
	    rman_get_size(base->mem_res[0]) < SDHCI_HOST_VERSION + 2 ||
	    rman_get_size(sc->cfg) < BCM2712_CFG_MIN_SIZE) {
		error = ENXIO;
		goto fail;
	}
	stage = "sw_sdio clock";
	error = clk_get_by_ofw_name(dev, 0, "sw_sdio", &sc->clock);
	if (error != 0)
		goto fail;
	error = clk_enable(sc->clock);
	if (error != 0)
		goto fail;
	sc->clock_enabled = true;
	error = clk_get_freq(sc->clock, &frequency);
	if (error != 0 || frequency == 0 || frequency > UINT32_MAX) {
		error = error != 0 ? error : ERANGE;
		goto fail;
	}
	if (OF_hasprop(node, "pinctrl-0")) {
		stage = "default pinctrl";
		error = fdt_pinctrl_configure_by_name_checked(dev, "default");
		if (error != 0)
			goto fail;
	}
	stage = "3.3V supplies";
	error = bcm2712_supply_get(dev, "vmmc-supply", &sc->vmmc);
	if (error == 0)
		error = bcm2712_supply_get(dev, "vqmmc-supply", &sc->vqmmc);
	if (error == 0)
		error = bcm2712_supply_power(&sc->vmmc, true);
	if (error == 0)
		error = bcm2712_supply_power(&sc->vqmmc, true);
	if (error != 0)
		goto fail;
	stage = "card-detect/write-protect GPIO";
	error = bcm2712_sdhci_pin(dev, "cd-gpios", &sc->cd_pin);
	if (error == 0 && !base->wp_disabled)
		error = bcm2712_sdhci_pin(dev, "wp-gpios", &sc->wp_pin);
	if (error != 0)
		goto fail;
	stage = "50MHz configuration";
	sc->cfg_ctrl = bus_read_4(sc->cfg, BCM2712_CFG_CTRL);
	sc->cfg_mode = bus_read_4(sc->cfg, BCM2712_CFG_MAX_50MHZ);
	sc->cfg_saved = true;
	bus_write_4(sc->cfg, BCM2712_CFG_MAX_50MHZ,
	    bcm2712_sdhci_50mhz_mode(sc->cfg_mode));
	if (OF_hasprop(node, "non-removable") || OF_hasprop(node, "broken-cd"))
		bus_write_4(sc->cfg, BCM2712_CFG_CTRL,
		    bcm2712_sdhci_force_present(sc->cfg_ctrl));
	bus_barrier(sc->cfg, 0, BCM2712_CFG_MIN_SIZE,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	if (!bcm2712_sdhci_is_50mhz(bus_read_4(sc->cfg, BCM2712_CFG_MAX_50MHZ))) {
		error = ENXIO;
		goto fail;
	}
	/* Keep the controller's actual base clock for SDHCI divisor math.
	 * The card-clock ceiling is imposed separately after slot setup.
	 */
	slot->max_clk = frequency;
	if (sc->cd_pin != NULL || OF_hasprop(node, "broken-cd"))
		slot->quirks |= SDHCI_QUIRK_POLL_CARD_PRESENT;
	if (OF_hasprop(node, "non-removable"))
		slot->quirks |= SDHCI_QUIRK_ALL_SLOTS_NON_REMOVABLE;
	if (!(bus_read_4(base->mem_res[0], SDHCI_CAPABILITIES) & SDHCI_CAN_VDD_330)) {
		error = ENXIO;
		goto fail;
	}
	stage = "SDHCI slot";
	error = sdhci_init_slot(dev, slot, 0);
	if (error != 0)
		goto fail;
	sc->slot_initialized = true;
	base->num_slots = 1;
	slot->host.f_max = MIN(slot->max_clk, sc->max_frequency);
	if (slot->host.f_min > slot->host.f_max) {
		error = ERANGE;
		goto fail;
	}
	if (width < 8)
		slot->host.caps &= ~MMC_CAP_8_BIT_DATA;
	if (width < 4)
		slot->host.caps &= ~MMC_CAP_4_BIT_DATA;
	slot->host.caps &= ~(MMC_CAP_SIGNALING_120 | MMC_CAP_SIGNALING_180);
	slot->host.caps |= MMC_CAP_SIGNALING_330;
	slot->host.ios.vccq = vccq_330;
	stage = "interrupt";
	rid = 0;
	base->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (base->irq_res == NULL) {
		error = ENXIO;
		goto fail;
	}
	error = bus_setup_intr(dev, base->irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, bcm2712_sdhci_intr, sc, &base->intrhand);
	if (error != 0)
		goto fail;
	sdhci_start_slot(slot);
	return (0);
fail:
	device_printf(dev, "bootstrap attach failed at %s: %d\n", stage, error);
	bcm2712_sdhci_release(dev);
	return (error);
}

static int
bcm2712_sdhci_detach(device_t dev)
{
	int error;

	error = bus_generic_detach(dev);
	if (error != 0)
		return (error);
	bcm2712_sdhci_release(dev);
	return (0);
}

static device_method_t bcm2712_sdhci_methods[] = {
	DEVMETHOD(device_probe,		bcm2712_sdhci_probe),
	DEVMETHOD(device_attach,		bcm2712_sdhci_attach),
	DEVMETHOD(device_detach,		bcm2712_sdhci_detach),
	DEVMETHOD(sdhci_read_4,		bcm2712_sdhci_read_4),
	DEVMETHOD(sdhci_set_clock,	bcm2712_sdhci_set_clock),
	DEVMETHOD(sdhci_get_card_present,	bcm2712_sdhci_present),
	DEVMETHOD(mmcbr_get_ro,		bcm2712_sdhci_readonly),
	DEVMETHOD(mmcbr_update_ios,	bcm2712_sdhci_update_ios),
	DEVMETHOD(mmcbr_switch_vccq,	bcm2712_sdhci_switch_vccq),
	DEVMETHOD_END
};
extern driver_t sdhci_fdt_driver;
DEFINE_CLASS_1(sdhci_bcm2712, bcm2712_sdhci_driver, bcm2712_sdhci_methods,
    sizeof(struct bcm2712_sdhci_softc), sdhci_fdt_driver);
DRIVER_MODULE(sdhci_bcm2712, simplebus, bcm2712_sdhci_driver, NULL, NULL);
SDHCI_DEPEND(sdhci_bcm2712);
#ifndef MMCCAM
MMC_DECLARE_BRIDGE(sdhci_bcm2712);
#endif

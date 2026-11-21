/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2012 Oleksandr Tymoshenko <gonzo@freebsd.org>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 * ACPI adaptation of the original bcm2835_sdhci.c 32-bit accessors.
 * Keep command/transfer and block-size/count writes paired; subword bus
 * operations are unsupported by this Arasan block. The FDT driver is retained.
 */
#ifndef _SDHCI_ACPI_BCM2835_H_
#define _SDHCI_ACPI_BCM2835_H_

static uint32_t
sdhci_acpi_bcm_rd4(struct sdhci_acpi_softc *sc, bus_size_t off)
{
	bus_barrier(sc->mem_res, 0, 0xff,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	return (bus_read_4(sc->mem_res, off));
}

static void
sdhci_acpi_bcm_wr4(struct sdhci_acpi_softc *sc, bus_size_t off, uint32_t val)
{
	bus_barrier(sc->mem_res, 0, 0xff,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	bus_write_4(sc->mem_res, off, val);
	/* Match the original driver's two-SD-clock-cycle write spacing. */
	if (sc->slot.clock > 0)
		DELAY(((2 * 1000000) / sc->slot.clock) + 1);
}

static uint8_t
sdhci_acpi_bcm_read_1(device_t dev, struct sdhci_slot *slot __unused, bus_size_t off)
{
	struct sdhci_acpi_softc *sc = device_get_softc(dev);
	uint32_t val = sdhci_acpi_bcm_rd4(sc, off & ~3);

	return ((val >> (off & 3)*8) & 0xff);
}

static uint16_t
sdhci_acpi_bcm_read_2(device_t dev, struct sdhci_slot *slot __unused, bus_size_t off)
{
	struct sdhci_acpi_softc *sc = device_get_softc(dev);
	uint32_t val32;

	/*
	 * Standard 32-bit handling of command and transfer mode, as
	 * well as block size and count.
	 */
	if ((off == SDHCI_BLOCK_SIZE || off == SDHCI_BLOCK_COUNT) &&
	    sc->need_update_blk)
		val32 = sc->blksz_and_count;
	else if (off == SDHCI_TRANSFER_MODE || off == SDHCI_COMMAND_FLAGS)
		val32 = sc->cmd_and_mode;
	else
		val32 = sdhci_acpi_bcm_rd4(sc, off & ~3);

	return ((val32 >> (off & 3)*8) & 0xffff);
}

static void
sdhci_acpi_bcm_write_1(device_t dev, struct sdhci_slot *slot __unused,
    bus_size_t off,
    uint8_t val)
{
	struct sdhci_acpi_softc *sc = device_get_softc(dev);
	uint32_t val32 = sdhci_acpi_bcm_rd4(sc, off & ~3);
	val32 &= ~(0xffU << (off & 3)*8);
	val32 |= ((uint32_t)val << (off & 3)*8);
	sdhci_acpi_bcm_wr4(sc, off & ~3, val32);
}

static void
sdhci_acpi_bcm_write_2(device_t dev, struct sdhci_slot *slot __unused,
    bus_size_t off,
    uint16_t val)
{
	struct sdhci_acpi_softc *sc = device_get_softc(dev);
	uint32_t val32;

	/*
	 * If we have a queued up 16bit value for blk size or count, use and
	 * update the saved value rather than doing any real register access.
	 * If we did not touch either since the last write, then read from
	 * register as at least block count can change.
	 * Similarly, if we are about to issue a command, always use the saved
	 * value for transfer mode as we can never write that without issuing
	 * a command.
	 */
	if ((off == SDHCI_BLOCK_SIZE || off == SDHCI_BLOCK_COUNT) &&
	    sc->need_update_blk)
		val32 = sc->blksz_and_count;
	else if (off == SDHCI_COMMAND_FLAGS)
		val32 = sc->cmd_and_mode;
	else
		val32 = sdhci_acpi_bcm_rd4(sc, off & ~3);

	val32 &= ~(0xffffU << (off & 3)*8);
	val32 |= ((uint32_t)val << (off & 3)*8);

	if (off == SDHCI_TRANSFER_MODE)
		sc->cmd_and_mode = val32;
	else if (off == SDHCI_BLOCK_SIZE || off == SDHCI_BLOCK_COUNT) {
		sc->blksz_and_count = val32;
		sc->need_update_blk = true;
	} else {
		if (off == SDHCI_COMMAND_FLAGS) {
			/* If we saved blk writes, do them now before cmd. */
			if (sc->need_update_blk) {
				sdhci_acpi_bcm_wr4(sc, SDHCI_BLOCK_SIZE, sc->blksz_and_count);
				sc->need_update_blk = false;
			}
			/* Always save cmd and mode registers. */
			sc->cmd_and_mode = val32;
		}
		sdhci_acpi_bcm_wr4(sc, off & ~3, val32);
	}
}

#endif /* _SDHCI_ACPI_BCM2835_H_ */

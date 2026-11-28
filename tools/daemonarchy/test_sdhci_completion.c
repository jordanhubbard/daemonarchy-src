/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Daemonarchy contributors
 * Included after verbatim production functions by test_sdhci_completion.py.
 */
#define BOUNDARY (512u * 1024u)
static unsigned failures;
static unsigned char arena[4 * BOUNDARY], bounce[BOUNDARY];
static struct mmc_data data;
static struct mmc_command cmd;
static struct mmc_request request;
static struct sdhci_slot slot;

static void setup(size_t len, size_t offset, int write)
{
    memset(arena, 0xa5, sizeof(arena));
    memset(bounce, 0x3c, sizeof(bounce));
    memset(&slot, 0, sizeof(slot));
    memset(&request, 0, sizeof(request));
    memset(syncs, 0, sizeof(syncs));
    starts = restarts = signal_writes = resets = platform_finishes = pios = 0;
    data = (struct mmc_data){.len = len, .data = arena,
        .flags = write ? MMC_DATA_WRITE : MMC_DATA_READ};
    cmd = (struct mmc_command){.data = &data};
    slot = (struct sdhci_slot){.curcmd = &cmd, .req = &request,
        .offset = offset, .sdma_bbufsz = BOUNDARY, .flags = SDHCI_USE_DMA,
        .cmd_done = 1, .dmamem = bounce, .intmask = SDHCI_INT_DMA_END,
        .paddr = 0x40280000};
}

static int bytes_are(size_t first, size_t end, unsigned char value)
{
    for (size_t i = first; i < end; i++)
        if (arena[i] != value) return 0;
    return 1;
}

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

static void completed(const char *name, size_t len, size_t offset, int write,
    uint32_t status, int response_pending)
{
    setup(len, offset, write);
    slot.cmd_done = !response_pending;
    sdhci_data_irq(&slot, status);
    check(name, slot.offset == offset && slot.data_done == 1 &&
        restarts == 0 && syncs[BUS_DMASYNC_PREREAD] == 0 &&
        syncs[BUS_DMASYNC_PREWRITE] == 0 &&
        syncs[write ? BUS_DMASYNC_POSTWRITE : BUS_DMASYNC_POSTREAD] == 1 &&
        starts == (unsigned)!response_pending &&
        signal_writes == (unsigned)response_pending &&
        (!response_pending || (slot.intmask & SDHCI_INT_RESPONSE)) &&
        bytes_are(0, offset, 0xa5) &&
        bytes_are(offset, len, write ? 0xa5 : 0x3c) &&
        bytes_are(len, sizeof(arena), 0xa5));
}

int main(void)
{
    const uint32_t both = SDHCI_INT_DMA_END | SDHCI_INT_DATA_END;
    completed("combined SCR eight bytes", 8, 0, 0, both, 0);
    completed("combined single sector", 512, 0, 0, both, 0);
    completed("combined full boundary", BOUNDARY, 0, 0, both, 0);
    completed("combined final partial buffer", BOUNDARY + 8, BOUNDARY, 0, both, 0);
    completed("combined final full buffer", 2 * BOUNDARY, BOUNDARY, 0, both, 0);
    completed("combined write eight bytes", 8, 0, 1, both, 0);
    completed("combined write full boundary", BOUNDARY, 0, 1, both, 0);
    completed("combined final partial write", BOUNDARY + 8, BOUNDARY, 1, both, 0);
    completed("completion before response", 8, 0, 0, both, 1);
    completed("ordinary read completion", 8, 0, 0, SDHCI_INT_DATA_END, 0);
    completed("ordinary write completion", 512, 0, 1, SDHCI_INT_DATA_END, 0);

    setup(2 * BOUNDARY, 0, 0);
    sdhci_data_irq(&slot, SDHCI_INT_DMA_END);
    check("intermediate read boundary reloads and masks last border",
        restarts == 1 && starts == 0 && slot.offset == BOUNDARY &&
        slot.data_done == 0 && syncs[BUS_DMASYNC_POSTREAD] == 1 &&
        syncs[BUS_DMASYNC_PREREAD] == 1 && signal_writes == 1 &&
        !(slot.intmask & SDHCI_INT_DMA_END) &&
        bytes_are(0, BOUNDARY, 0x3c) && bytes_are(BOUNDARY, sizeof(arena), 0xa5));
    memset(bounce, 0x7e, sizeof(bounce));
    sdhci_data_irq(&slot, SDHCI_INT_DATA_END);
    check("read completion after boundary preserves both buffers",
        starts == 1 && restarts == 1 && slot.data_done == 1 &&
        bytes_are(0, BOUNDARY, 0x3c) && bytes_are(BOUNDARY, 2 * BOUNDARY, 0x7e) &&
        bytes_are(2 * BOUNDARY, sizeof(arena), 0xa5));

    setup(3 * BOUNDARY, 0, 1);
    sdhci_data_irq(&slot, SDHCI_INT_DMA_END);
    check("intermediate write boundary reloads without early mask",
        restarts == 1 && starts == 0 && slot.offset == BOUNDARY &&
        slot.data_done == 0 && syncs[BUS_DMASYNC_POSTWRITE] == 1 &&
        syncs[BUS_DMASYNC_PREWRITE] == 1 && signal_writes == 0 && bounce[0] == 0xa5 &&
        bounce[BOUNDARY - 1] == 0xa5 && bytes_are(0, sizeof(arena), 0xa5));

    setup(8, 0, 0);
    slot.flags = PLATFORM_DATA_STARTED;
    sdhci_data_irq(&slot, both);
    check("combined platform completion bypasses SDMA reload",
        platform_finishes == 1 && !(slot.flags & PLATFORM_DATA_STARTED) &&
        slot.offset == 0 && restarts == 0 && starts == 0 && bytes_are(0, sizeof(arena), 0xa5));

    setup(8, 0, 0);
    sdhci_data_irq(&slot, both | SDHCI_INT_DATA_CRC);
    check("CRC error resets without boundary reload", cmd.error == MMC_ERR_BADCRC &&
        resets == 2 && starts == 1 && restarts == 0 &&
        (slot.retune_req & SDHCI_RETUNE_REQ_RESET) && bytes_are(8, sizeof(arena), 0xa5));

    setup(8, 0, 0);
    cmd.data = NULL;
    cmd.flags = MMC_RSP_BUSY;
    sdhci_data_irq(&slot, SDHCI_INT_DATA_END);
    check("busy-only completion", slot.data_done && starts == 1 && restarts == 0);

    setup(8, 0, 0);
    cmd.data = NULL;
    sdhci_data_irq(&slot, both);
    check("no active data refuses interrupt", slot.data_done == 0 && starts == 0 && restarts == 0);
    slot.curcmd = NULL;
    sdhci_data_irq(&slot, both);
    check("no active command refuses interrupt", starts == 0 && restarts == 0);
    return failures != 0;
}

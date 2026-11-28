/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors */
/* Independent protocol expectations for extracted production code, not a SIM. */
static unsigned passes, failures;
#define CHECK(name, expression) do { \
    if (expression) { puts("PASS " name); passes++; } \
    else { puts("FAIL " name); failures++; } \
} while (0)
#define HOST (1u << 20)
#define CARD (HOST | (1u << 21))
#define READY (1u << 31)
#define MEMORY (1u << 27)
static struct device dev;
static struct cam_path path = { &dev };
static struct cam_periph periph = { &path };
static mmcprobe_softc sc;
static union ccb request;
#define PARAMS (&dev.mmc_ident_data)
static void reset(void) {
    memset(&dev, 0, sizeof(dev)); memset(&sc, 0, sizeof(sc));
    memset(&request, 0, sizeof(request));
    sc.action = PROBE_SDIO_INIT; sc.host_ocr = HOST; sc.periph = &periph;
    PARAMS->card_features = OTHER_FEATURES;
    now = 10 * SBT_1S; delays = debug_calls = 0;
}
static void respond(unsigned fn, uint32_t bits) {
    request.ccb_h.status = CAM_REQ_CMP | 0x100;
    request.mmcio.cmd.error = MMC_ERR_NONE;
    request.mmcio.cmd.resp[0] = (fn << 28) | bits;
    complete_cmd(&sc, &path, &request);
}
static int clean_memory(void) {
    return sc.action == PROBE_SEND_APP_OP_COND && PARAMS->io_ocr == 0 &&
        PARAMS->sdio_func_count == 0 && PARAMS->card_features == OTHER_FEATURES;
}
static void powered(void) {
    reset(); send_cmd(&sc, PARAMS, &request); respond(1, CARD);
    send_cmd(&sc, PARAMS, &request);
}
int main(void) {
    reset(); send_cmd(&sc, PARAMS, &request);
    CHECK("query has zero OCR", request.mmcio.cmd.arg == 0);
    CHECK("query does not start power timer", sc.sdio_tries == 0 && sc.sdio_started == 0);
    CHECK("CMD5 is broadcast with response", request.mmcio.cmd.opcode == 5 &&
        request.mmcio.cmd.flags == (MMC_RSP_R4 | MMC_CMD_BCR));
    CHECK("command has bounded timeout", request.ccb_h.timeout == 1000);
    CHECK("standard CCB fields retained", request.ccb_h.func_code == XPT_MMC_IO &&
        request.ccb_h.flags == CAM_DIR_OUT && request.ccb_h.retry_count == 0 &&
        request.ccb_h.cbfcnp == mmcprobe_done && request.mmcio.stop.opcode == 0);
    respond(1, CARD | READY);
    CHECK("query selects only compatible voltage", PARAMS->io_ocr == HOST);
    CHECK("query never publishes IO ready", sc.action == PROBE_SDIO_INIT &&
        PARAMS->sdio_func_count == 0 && PARAMS->card_features == OTHER_FEATURES);
    send_cmd(&sc, PARAMS, &request);
    CHECK("power request starts timer once", sc.sdio_tries == 1 && sc.sdio_started == now);
    CHECK("power request sends compatible OCR", request.mmcio.cmd.arg == HOST);
    respond(1, CARD);
    CHECK("not-ready response stays in CMD5", sc.action == PROBE_SDIO_INIT);
    CHECK("not-ready response publishes no functions", PARAMS->sdio_func_count == 0 &&
        PARAMS->card_features == OTHER_FEATURES);
    CHECK("one bounded delay even with debug enabled", delays == 1);
    now += SBT_1S / 10; send_cmd(&sc, PARAMS, &request);
    CHECK("retry preserves start time", sc.sdio_tries == 2 && sc.sdio_started == 10 * SBT_1S);
    respond(1, CARD | READY);
    CHECK("ready IO card proceeds to RCA", sc.action == PROBE_SEND_RELATIVE_ADDR &&
        PARAMS->sdio_func_count == 1 && PARAMS->io_ocr == HOST &&
        PARAMS->card_features == (OTHER_FEATURES | CARD_FEATURE_SDIO));
    powered(); respond(2, HOST | MEMORY | READY);
    CHECK("ready combo card probes memory", sc.action == PROBE_SEND_APP_OP_COND &&
        PARAMS->sdio_func_count == 2 && (PARAMS->card_features & CARD_FEATURE_SDIO));
    for (unsigned fn = 1; fn <= 7; fn++) {
        powered(); respond(fn, HOST | READY);
        char name[60]; snprintf(name, sizeof(name), "%u functions retained", fn);
        if (PARAMS->sdio_func_count == fn) { printf("PASS %s\n", name); passes++; }
        else { printf("FAIL %s\n", name); failures++; }
    }
    reset(); send_cmd(&sc, PARAMS, &request); respond(0, HOST | MEMORY | READY);
    CHECK("ordinary memory SD takes fallback without delay", clean_memory() && delays == 0);
    reset(); send_cmd(&sc, PARAMS, &request); respond(1, 0);
    CHECK("zero OCR terminates IO probe", clean_memory() && delays == 0);
    reset(); send_cmd(&sc, PARAMS, &request); respond(1, 0xf0);
    CHECK("reserved OCR bits are not voltage", clean_memory());
    reset(); sc.host_ocr = 1u << 7;
    send_cmd(&sc, PARAMS, &request); respond(1, 1u << 7);
    CHECK("shared host low-voltage bit is reserved in SDIO OCR", clean_memory());
    reset(); send_cmd(&sc, PARAMS, &request); respond(1, (1u << 21));
    CHECK("incompatible voltage takes memory fallback", clean_memory());
    powered(); respond(1, (1u << 21) | READY);
    CHECK("changed incompatible voltage is refused", clean_memory());
    powered(); request.mmcio.cmd.arg = 1u << 21; respond(1, HOST | READY);
    CHECK("response must overlap requested voltage", clean_memory());
    powered(); now += SBT_1S - 1; respond(1, HOST);
    CHECK("not-ready before deadline can retry", sc.action == PROBE_SDIO_INIT && delays == 1);
    powered(); now += SBT_1S; respond(1, HOST);
    CHECK("not-ready at deadline falls back without delay", clean_memory() && delays == 0);
    powered(); sc.sdio_tries = 1000; respond(1, HOST);
    CHECK("retry ceiling bounds stalled clock", clean_memory() && delays == 0);
    powered(); sc.sdio_started = INT64_MAX - SBT_1S / 2;
    now = INT64_MIN + SBT_1S / 2; respond(1, HOST);
    CHECK("elapsed deadline tolerates signed timer wrap", clean_memory());
    powered(); PARAMS->sdio_func_count = 7; PARAMS->card_features |= CARD_FEATURE_SDIO;
    request.mmcio.cmd.error = MMC_ERR_TIMEOUT;
    complete_cmd(&sc, &path, &request);
    CHECK("command failure clears stale IO metadata", clean_memory());
    powered(); PARAMS->sdio_func_count = 7; PARAMS->card_features |= CARD_FEATURE_SDIO;
    request.mmcio.cmd.resp[0] = READY | HOST | (7u << 28);
    request.ccb_h.status = 2; request.mmcio.cmd.error = MMC_ERR_NONE;
    complete_cmd(&sc, &path, &request);
    CHECK("transport failure clears stale IO metadata", clean_memory());
#ifdef HAVE_RESTART_HELPER
    powered(); PARAMS->sdio_func_count = 7; PARAMS->card_features |= CARD_FEATURE_SDIO;
    mmcprobe_sdio_restart(&sc, PARAMS);
    CHECK("restart clears timer and IO state only", sc.sdio_started == 0 &&
        sc.sdio_tries == 0 && PARAMS->io_ocr == 0 && PARAMS->sdio_func_count == 0 &&
        PARAMS->card_features == OTHER_FEATURES);
#endif
    printf("%u passed, %u failed\n", passes, failures);
    return failures != 0;
}

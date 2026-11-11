/* SPDX-License-Identifier: BSD-2-Clause */
/* Diagnostic-only BCM2711 simulator profile; no BCM2712 device binding. */
#ifndef _BCM2711_DMA_TEST_H_
#define _BCM2711_DMA_TEST_H_
#define BCM2711_DMA_TEST_OFFSET (UINT64_C(1) << 36)
#define BCM2711_DMA_TEST_WINDOW_LOW 0x12U
#define BCM2711_DMA_TEST_WINDOW_HIGH 0x10U
struct bcm2711_dma_test_witness {
	uint64_t callbacks, bytes, covered_bytes, errors, first_device, last_device;
};
int bcm2711_dma_test_requested(void);
int bcm2711_dma_test_create(bus_dma_tag_t, bus_dma_tag_t *);
int bcm2711_dma_test_publish(device_t, bus_dma_tag_t, bus_dma_tag_t,
    uint32_t, uint32_t);
int bcm2711_dma_test_begin(void *, size_t);
void bcm2711_dma_test_end(struct bcm2711_dma_test_witness *);
void bcm2711_dma_test_usb_load(void *, size_t, bus_dma_segment_t *, int, int);
void bcm2711_dma_test_ring(const char *, uint64_t);
int bcm2711_dma_test_tags(bus_dma_tag_t *, bus_dma_tag_t *);
#endif

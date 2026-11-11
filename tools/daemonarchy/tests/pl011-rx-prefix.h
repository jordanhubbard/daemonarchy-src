/* SPDX-License-Identifier: BSD-2-Clause */
/* Typed host services; no native UART timing, interrupt or TTY qualification. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/types.h>
#define __unused __attribute__((unused))
struct uart_softc;
struct uart_bas { unsigned unused; };
struct tty { struct uart_softc *sc; struct { unsigned c_cflag; } t_termios; };
struct uart_softc {
    struct uart_bas sc_bas;
    void *sc_hwmtx, *sc_softih;
    bool sc_opened, sc_txbusy, sc_leaving, sc_polled, sc_testintr;
    bool sc_isquelch, sc_hwiflow;
    uint16_t *sc_rxbuf;
    int sc_rxbufsz, sc_rxget, sc_rxput, sc_rxoverruns;
    int sc_timer;
    uint32_t sc_ttypend;
    union { struct { struct tty *tp; } u_tty; } sc_u;
};
static unsigned lock_depth, register_writes, barriers, data_reads, mis_reads;
static unsigned soft_schedules, rint_done, setsig_calls, last_sig, checks;
static bool hard_active, use_baseline, bounded_old;
static jmp_buf witness;
static uint32_t input[8192];
static unsigned input_count, input_pos;
static uint32_t live_imsc;
static unsigned irq_kind;
static unsigned delivered[8192], delivered_errors[8192], delivered_count;
static unsigned tty_limit = 8192;
static void uart_lock(void *p) { (void)p; assert(lock_depth == 0); lock_depth = 1; }
static void uart_unlock(void *p) { (void)p; assert(lock_depth == 1); lock_depth = 0; }
static void uart_barrier(struct uart_bas *bas) { (void)bas; assert(lock_depth == 1); barriers++; }
static uint32_t __uart_getreg(struct uart_bas *, unsigned);
static void __uart_setreg(struct uart_bas *bas, unsigned reg, uint32_t value) {
    (void)bas; assert(lock_depth == 1); register_writes++;
    if (reg == 0x0e) live_imsc = value;
}
static int receive_dispatch(struct uart_softc *);
static int uart_pl011_bus_ipend(struct uart_softc *);
static int uart_pl011_bus_flush(struct uart_softc *, int);
static int uart_pl011_bus_setsig(struct uart_softc *, int);
#define UART_RECEIVE(sc) receive_dispatch(sc)
#define UART_IPEND(sc) uart_pl011_bus_ipend(sc)
#define UART_FLUSH(sc,what) uart_pl011_bus_flush(sc,what)
static int record_setsig(struct uart_softc *sc, int sig) {
    setsig_calls++; last_sig = sig; return uart_pl011_bus_setsig(sc, sig);
}
#define UART_SETSIG(sc,sig) record_setsig(sc,sig)
static int atomic_cmpset_32(uint32_t *p, uint32_t old, uint32_t value) {
    if (*p != old)
        return 0;
    *p = value;
    return 1;
}
static uint32_t atomic_readandclear_32(uint32_t *p) { uint32_t old=*p; *p=0; return old; }
static void swi_sched(void *p, int flags) { (void)p; (void)flags; soft_schedules++; }
static void tty_lock(struct tty *tp) { (void)tp; assert(!hard_active); }
static void tty_unlock(struct tty *tp) { (void)tp; }
static struct uart_softc *tty_softc(struct tty *tp) { return tp->sc; }
static int ttydisc_rint(struct tty *tp, int c, int err) {
    (void)tp; assert(!hard_active);
    if (delivered_count >= tty_limit) return 1;
    delivered[delivered_count]=c; delivered_errors[delivered_count++]=err; return 0;
}
static void ttydisc_rint_done(struct tty *tp) { (void)tp; rint_done++; }
static void ttydisc_modem(struct tty *tp, int state) { (void)tp; (void)state; assert(0); }
static void uart_tty_outwakeup(struct tty *tp) { (void)tp; assert(0); }
static int uart_intr_sigchg(void *sc) { (void)sc; assert(0); return 0; }
typedef void callout_func_t(void *);
static void callout_reset(void *p, int ticks, callout_func_t *fn, void *arg) {
    (void)p; (void)ticks; (void)fn; (void)arg; assert(0);
}
static int hz=100, uart_poll_freq=50;

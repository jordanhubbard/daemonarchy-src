/* SPDX-License-Identifier: BSD-2-Clause */
static uint16_t buffer[1024];
static struct uart_pl011_softc psc;
static struct tty tty;
static unsigned newbyte(unsigned i) { return (i * 13 + 7) & 255; }
static void check(bool ok, const char *label) {
    if (!ok) { fprintf(stderr,"FAIL: %s\n",label); abort(); } checks++;
}
static void reset(unsigned size, unsigned get, unsigned put, unsigned queued) {
    memset(&psc,0,sizeof(psc)); memset(&tty,0,sizeof(tty));
    for (unsigned i=0;i<1024;i++) buffer[i]=0xbeef;
    psc.base.sc_rxbuf=buffer; psc.base.sc_rxbufsz=size;
    psc.base.sc_rxget=get; psc.base.sc_rxput=put; psc.base.sc_opened=true;
    psc.base.sc_hwmtx=&lock_depth; psc.base.sc_u.u_tty.tp=&tty; tty.sc=&psc.base;
    for (unsigned i=0;i<queued;i++) buffer[(get+i)%size]=(0xa0+i)&255;
    live_imsc=psc.imsc=UART_RXREADY|RIS_RTIM|UART_TXEMPTY;
    irq_kind=UART_RXREADY; lock_depth=register_writes=barriers=data_reads=mis_reads=0;
    soft_schedules=rint_done=setsig_calls=last_sig=delivered_count=0;
    input_count=input_pos=0; tty_limit=8192; hard_active=use_baseline=bounded_old=false;
}
static void feed(unsigned count) {
    assert(count<8192); input_pos=0; input_count=count;
    for (unsigned i=0;i<count;i++) input[i]=newbyte(i);
}
static int filter(void) {
    hard_active=true; int result=uart_intr(&psc.base); hard_active=false; return result;
}
int main(void) {
    /* The intact old receive + shared filter cannot yield to its queued SWI. */
    reset(384,0,0,0); feed(450); use_baseline=bounded_old=true;
    if (setjmp(witness)==0) { (void)filter(); check(false,"old filter unexpectedly returned"); }
    check(input_pos==383 && psc.base.sc_rxput==383 && input_count-input_pos==67,
        "old bounded witness preserves finite pending FIFO after SW ring fills");
    check(soft_schedules==1 && delivered_count==0 && hard_active,
        "old hard filter resamples RX without allowing scheduled SWI");
    lock_depth=0; hard_active=false;
    for (unsigned n=0;n<sizeof(vectors)/sizeof(vectors[0]);n++) {
        const struct vector *v=&vectors[n]; reset(v->size,v->get,v->put,v->queued); feed(v->total);
        unsigned old_get=psc.base.sc_rxget, expected_free=v->size-1-v->queued;
        unsigned accepted=v->total<expected_free?v->total:expected_free;
        check(filter()==(v->total?FILTER_HANDLED:FILTER_STRAY),"finite actual generic filter terminates");
        check(input_pos==v->total && data_reads==v->total,"all finite hardware data consumed");
        check(mis_reads<=2 && lock_depth==0,"finite IRQ quiescence and hardware unlock");
        check(psc.base.sc_rxput==(int)v->expected_put && psc.base.sc_rxget==(int)old_get,
            "sealed producer/consumer positions");
        check(register_writes==0 && live_imsc==psc.imsc,"receive does not mask RX or change TX state");
        check(barriers==v->discard,"discard reads ordered by bus barriers");
        check(buffer[v->size]==0xbeef,"ring-bound canary unchanged");
        for (unsigned i=0;i<v->queued;i++) check(buffer[(old_get+i)%v->size]==((0xa0+i)&255),"old queued prefix retained");
        for (unsigned i=0;i<accepted;i++) check(buffer[(old_get+v->queued+i)%v->size]==newbyte(i),"new queued prefix retained");
        if(v->discard) check(buffer[psc.base.sc_rxput]==UART_STAT_OVERRUN,"existing reserved overflow sentinel retained");
        uart_tty_intr(&psc.base);
        check(delivered_count==v->queued+accepted,"actual SWI drains only enqueued prefix, not reserved sentinel");
        check(uart_rx_empty(&psc.base),"SWI ring empty after finite input");
        feed(3); unsigned prior=delivered_count; check(filter()==FILTER_HANDLED,"next finite input interrupts normally");
        uart_tty_intr(&psc.base);check(delivered_count==prior+3,"next short input reaches TTY after overflow");
    }
    reset(384,0,0,0);feed(450);psc.base.sc_isquelch=true;tty.t_termios.c_cflag=CRTS_IFLOW;
    check(filter()==FILTER_HANDLED,"squelched finite IRQ terminates");uart_tty_intr(&psc.base);
    check(delivered_count==0 && psc.base.sc_rxput==383,"squelch retains queued data");
    uart_tty_inwakeup(&tty);check(!psc.base.sc_isquelch && setsig_calls==1 && last_sig==(SER_DRTS|SER_RTS),"actual input wakeup restores existing logical RTS/SWI path");
    uart_tty_intr(&psc.base);check(delivered_count==383,"wakeup consumes preserved prefix");
    reset(384,0,0,0);feed(3);tty.t_termios.c_cflag=CRTS_IFLOW;tty_limit=1;
    check(filter()==FILTER_HANDLED,"TTY pressure preparation");uart_tty_intr(&psc.base);
    check(psc.base.sc_isquelch && delivered_count==1 && psc.base.sc_rxget==1 && setsig_calls==1 && last_sig==SER_DRTS,"actual rint failure squelches without consuming rejected character");
    feed(450);check(filter()==FILTER_HANDLED,"already squelched backlog still quiesces finite input");
    check(buffer[1]==newbyte(1) && buffer[2]==newbyte(2),"TTY rejected prefix retained across later overflow");
    tty_limit=8192;uart_tty_inwakeup(&tty);uart_tty_intr(&psc.base);
    check(delivered_count==384 && !psc.base.sc_isquelch && setsig_calls==2,"pressure release schedules and drains retained ring");
    reset(384,0,0,0);feed(450);psc.base.sc_opened=false;
    check(filter()==FILTER_HANDLED && psc.base.sc_rxget==psc.base.sc_rxput && !soft_schedules,"closed port quiesces and core discards ring");
    psc.base.sc_opened=true;feed(3);check(filter()==FILTER_HANDLED,"reopened port receives");uart_tty_intr(&psc.base);check(delivered_count==3,"reopen delivers next short input");
    reset(384,0,0,0);feed(2);input[0]|=DR_FE;input[1]|=DR_PE;
    check(filter()==FILTER_HANDLED,"normal error bytes interrupt");
    check(buffer[0]==(newbyte(0)|UART_STAT_FRAMERR) && buffer[1]==(newbyte(1)|UART_STAT_PARERR),"existing FE/PE status translation retained");
    reset(384,0,0,0);feed(450);irq_kind=RIS_RTIM;
    check(filter()==FILTER_HANDLED && input_pos==450,"typed finite timeout-pending source drains; not hardware timeout ACK proof");
    printf("PASS: %u whole PL011 receive/core IRQ/ring/TTY host checks; finite loss policy, not native UART qualification\n",checks);
    return 0;
}

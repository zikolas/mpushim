/* PMISR - do trapped MPU I/O from a TIMER INTERRUPT HANDLER, the way a
 * game's music driver (DMX et al) does, rather than from the main thread.
 * Every other test so far drives the trap from the main thread and passes;
 * this isolates "trapping from an ISR context" as the remaining difference.
 *
 * Run under a resident MPUSHIM.  It hooks the PM timer vector, writes a
 * MIDI-ish byte + polls status on every tick for ~10 seconds, prints a live
 * count, then unhooks.  (C) 2026 zikolas, GNU GPL v2 (see COPYING).
 */
#include <stdio.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <crt0.h>
#include <sys/farptr.h>

int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY | _CRT0_FLAG_NONMOVE_SBRK;

#define DATA 0x330
#define STAT 0x331

volatile unsigned long isr_hits = 0;
volatile int isr_burst = 200;      /* trapped pairs per tick (like a music ISR) */
volatile int main_too  = 0;        /* also hammer from the MAIN thread = NESTING */
static _go32_dpmi_seginfo old_isr, new_isr;

static void timer_isr(void)
{
    int i;
    for (i = 0; i < isr_burst; i++) {
        outportb(DATA, 0xFE);      /* trapped OUT from interrupt context  */
        (void)inportb(STAT);       /* trapped IN  from interrupt context  */
    }
    isr_hits++;
}

static unsigned long ticks(void) { return _farpeekl(_dos_ds, 0x46c); }

int main(int argc, char **argv)
{
    unsigned long t0, last = 0;
    /* repair CR0 after DJGPP's 0E01h startup call (see MPUSHIM.C bug 1) */
    __dpmi_set_coprocessor_emulation(0);
    if (argc > 1) {
        int n = 0; const char *p = argv[1];
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        if (n > 0) isr_burst = n;
    }
    if (argc > 2) main_too = 1;
    printf("PMISR: ISR %d pairs/tick (~%d traps/sec)%s\n",
           isr_burst, isr_burst * 2 * 18,
           main_too ? " + MAIN-THREAD traps = NESTED dispatch" : "");
    fflush(stdout);
    fflush(stdout);

    _go32_dpmi_lock_code((void *)timer_isr, 512);
    _go32_dpmi_lock_data((void *)&isr_hits, 8);
    _go32_dpmi_lock_data((void *)&isr_burst, 8);
    _go32_dpmi_lock_data((void *)&main_too, 8);

    _go32_dpmi_get_protected_mode_interrupt_vector(0x08, &old_isr);
    new_isr.pm_offset   = (unsigned long)timer_isr;
    new_isr.pm_selector = _go32_my_cs();
    if (_go32_dpmi_chain_protected_mode_interrupt_vector(0x08, &new_isr) != 0) {
        printf("PMISR: could not hook the timer\n");
        return 1;
    }
    t0 = ticks();
    while (ticks() - t0 < 182) {          /* ~10 seconds */
        unsigned long n = isr_hits;
        if (main_too) {                   /* main-thread traps, interruptible
                                           * by the ISR's traps = nesting */
            int k;
            for (k = 0; k < 200; k++) {
                outportb(DATA, 0xFE);
                (void)inportb(STAT);
            }
        }
        if (n - last >= 18) {             /* about once a second */
            last = n;
            printf("  ticks handled: %lu\n", n);
            fflush(stdout);
        }
    }
    _go32_dpmi_set_protected_mode_interrupt_vector(0x08, &old_isr);
    printf("PMISR: survived, %lu ISR-context trap pairs\n", isr_hits);
    return 0;
}

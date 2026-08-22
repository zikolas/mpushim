/* PMHOG - imitate the resource footprint of a big DPMI client (DOS4GW game):
 * grab a large memory block, touch every page, and allocate a pile of LDT
 * descriptors; then exit.  Run PMPOKE before and after: if the facade works
 * before and fails after, a big client is invalidating the resident trap
 * handler's memory or selectors.  MIT (c) 2026 zikolas.
 */
#include <stdio.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>

int main(void)
{
    __dpmi_meminfo m;
    int i, n = 0;
    unsigned long mb = 8;
    /* repair CR0 after DJGPP's 0E01h startup call (see MPUSHIMP.C bug 1) */
    __dpmi_set_coprocessor_emulation(0);
    m.address = 0;
    m.size = mb * 1024UL * 1024UL;
    if (__dpmi_allocate_memory(&m) != 0) {
        printf("PMHOG: %lu MB alloc FAILED\n", mb);
    } else {
        unsigned long off;
        int sel;
        printf("PMHOG: got %lu MB at linear %08lX, touching it...\n",
               mb, m.address);
        sel = __dpmi_allocate_ldt_descriptors(1);
        if (sel < 0) {
            printf("PMHOG: no descriptor for the block\n");
        } else {
            __dpmi_set_segment_base_address(sel, m.address);
            __dpmi_set_segment_limit(sel, m.size - 1);
            for (off = 0; off < m.size; off += 4096)
                _farpokeb(sel, off, 0x5A);
            printf("PMHOG: touched %lu pages\n", m.size / 4096);
        }
    }
    for (i = 0; i < 64; i++)
        if (__dpmi_allocate_ldt_descriptors(1) > 0) n++;
    printf("PMHOG: allocated %d LDT descriptors\n", n);
    printf("PMHOG: exiting WITHOUT freeing anything\n");
    return 0;
}

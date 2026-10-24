/* Generated into the experimental kernel tree by k93-instrument-kernel.py. */
#ifndef _LINUX_K93_TRACE_H
#define _LINUX_K93_TRACE_H
static inline void k93_trace_deadline(unsigned int seconds)
{
#ifdef K93_INIT_STAGE_TRACE
    /* Diagnostic-only identity mapping, retained by prepare_page_table. */
    *(volatile unsigned int *)0x3f103024ul = seconds * 24000000u;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
static inline void k93_boot_stage(unsigned int seconds)
{
#ifndef K93_LATE_STAGE_TRACE
    k93_trace_deadline(seconds);
#endif
}
static inline void k93_late_stage(unsigned int seconds)
{
#ifdef K93_LATE_STAGE_TRACE
    k93_trace_deadline(seconds);
#endif
}
#endif

#!/usr/bin/env python3
"""
Apply / verify P105AP bring-up hacks in build/linux (idempotent).

build-kernel.sh runs this before every compile.  Fails fast if init/main.c
still contains the obsolete rest_clone marker.
"""

from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# (file, old, new, description) — skipped if new already present
REPLACEMENTS = [
    (
        "init/main.c",
        """#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("rest_init");
\tp105_fb_dbg("rest_clone");
#endif
\tpid = user_mode_thread(kernel_init, NULL, CLONE_FS);
""",
        """#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("rest_init");
\tp105_fb_dbg("pre_spawn");
\tpid = apple_s5l_rest_spawn_init(kernel_init, NULL, CLONE_FS);
#else
\tpid = user_mode_thread(kernel_init, NULL, CLONE_FS);
#endif
""",
        "rest_init: replace obsolete rest_clone bisect path",
    ),
    (
        "init/main.c",
        """#ifdef CONFIG_ARCH_APPLE_S5L
void p105_fb_dbg(const char *msg);
#endif

static noinline void __ref __noreturn rest_init(void)
""",
        """#ifdef CONFIG_ARCH_APPLE_S5L
void p105_fb_dbg(const char *msg);
pid_t __init apple_s5l_rest_spawn_init(int (*fn)(void *), void *arg,
\t\t\t\t       unsigned long flags);
#endif

static noinline void __ref __noreturn rest_init(void)
""",
        "rest_init: declare apple_s5l_rest_spawn_init",
    ),
    (
        "init/main.c",
        '\t\tlocal_irq_enable();\n\t}\n\tWARN(msgbuf[0], "initcall',
        '#ifdef CONFIG_ARCH_APPLE_S5L\n'
        '\t\tif (!early_boot_irqs_disabled)\n'
        '#endif\n'
        '\t\t\tlocal_irq_enable();\n'
        '\t}\n'
        '\tWARN(msgbuf[0], "initcall',
        "do_one_initcall: keep IRQs off during early boot",
    ),
    (
        "init/main.c",
        """\t\tp105_fb_dbg("irq_arm");
\t\tapple_aic1_early_irq_escape = false;
\t}
#else
""",
        """\t\tp105_fb_dbg("irq_arm");
\t\tlocal_irq_disable();
\t\tearly_boot_irqs_disabled = true;
\t\tp105_fb_dbg("irq_frz");
\t}
#else
""",
        "main.c: re-freeze IRQs after irq_arm (from escape-clear)",
    ),
    (
        "init/main.c",
        """\t\tp105_fb_dbg("irq_arm");
\t\t/* Keep early_irq_escape: unlock_irq/cpsie still see sticky nIRQ */
\t}
#else
""",
        """\t\tp105_fb_dbg("irq_arm");
\t\tlocal_irq_disable();
\t\tearly_boot_irqs_disabled = true;
\t\tp105_fb_dbg("irq_frz");
\t}
#else
""",
        "main.c: re-freeze IRQs after irq_arm (kc_wake)",
    ),
    (
        "kernel/fork.c",
        """#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_wake");
#endif
\twake_up_new_task(p);

\t/* forking complete and child started to run, tell ptracer */
\tif (unlikely(trace))
\t\tptrace_event_pid(trace, pid);

\tif (clone_flags & CLONE_VFORK) {
\t\tif (!wait_for_vfork_done(p, &vfork))
\t\t\tptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
\t}

\tput_pid(pid);
\treturn nr;
}
""",
        """#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_wake");
#endif
\twake_up_new_task(p);
#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_woke");
#endif

\t/* forking complete and child started to run, tell ptracer */
\tif (unlikely(trace))
\t\tptrace_event_pid(trace, pid);

\tif (clone_flags & CLONE_VFORK) {
\t\tif (!wait_for_vfork_done(p, &vfork))
\t\t\tptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
\t}

#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_put");
#endif
\tput_pid(pid);
#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_out");
#endif
\treturn nr;
}
""",
        "fork.c: kc_woke/put/out around wake and put_pid",
    ),
    (
        "kernel/fork.c",
        """#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_woke");
#endif

\t/* forking complete and child started to run, tell ptracer */
\tif (unlikely(trace))
\t\tptrace_event_pid(trace, pid);

\tif (clone_flags & CLONE_VFORK) {
\t\tif (!wait_for_vfork_done(p, &vfork))
\t\t\tptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
\t}

\tput_pid(pid);
\treturn nr;
}
""",
        """#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_woke");
#endif

\t/* forking complete and child started to run, tell ptracer */
\tif (unlikely(trace))
\t\tptrace_event_pid(trace, pid);

\tif (clone_flags & CLONE_VFORK) {
\t\tif (!wait_for_vfork_done(p, &vfork))
\t\t\tptrace_event_pid(PTRACE_EVENT_VFORK_DONE, pid);
\t}

#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_put");
#endif
\tput_pid(pid);
#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg("kc_out");
#endif
\treturn nr;
}
""",
        "fork.c: add kc_put/out when kc_woke already present",
    ),
    (
        "init/main.c",
        """\t\tvoid apple_aic1_quiesce(void);
\t\tvoid apple_aic1_hw_quiesce(void);
\t\tvoid apple_aic1_enable(void);
\t\tvoid apple_aic1_arm_cpu0(void);

\t\tp105_fb_dbg("irq_quiesce");
\t\tapple_aic1_quiesce();
\t\tapple_aic1_hw_quiesce();
\t\tapple_aic1_enable();
\t\tp105_fb_dbg("irq_try");
\t\tearly_boot_irqs_disabled = false;
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("irq_on");
\t\tlocal_irq_disable();
\t\tapple_aic1_arm_cpu0();
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("irq_arm");
\t}
#else
""",
        """\t\tvoid apple_aic1_quiesce(void);
\t\tvoid apple_aic1_hw_quiesce(void);
\t\tvoid apple_aic1_enable(void);
\t\tvoid apple_aic1_arm_cpu0(void);
\t\textern bool apple_aic1_early_irq_escape;

\t\tp105_fb_dbg("irq_quiesce");
\t\tapple_aic1_quiesce();
\t\tapple_aic1_hw_quiesce();
\t\t/* CFG off; keep early_irq_escape for sticky nIRQ */
\t\tp105_fb_dbg("irq_try");
\t\tearly_boot_irqs_disabled = false;
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("irq_on");
\t\tlocal_irq_disable();
\t\tapple_aic1_enable();
\t\tapple_aic1_arm_cpu0();
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("irq_arm");
\t}
#else
""",
        "main.c: CFG off at irq_try; enable+arm after irq_on",
    ),
    (
        "arch/arm/kernel/entry-armv.S",
        """\tirq_handler from_user=0

#ifdef CONFIG_PREEMPTION
\tldr\tr8, [tsk, #TI_PREEMPT]\t\t@ get preempt count
\tldr\tr0, [tsk, #TI_FLAGS]\t\t@ get flags
\tteq\tr8, #0\t\t\t\t@ if preempt count != 0
\tmovne\tr0, #0\t\t\t\t@ force flags to 0
\ttst\tr0, #_TIF_NEED_RESCHED
\tblne\tsvc_preempt
#endif

\tsvc_exit r5, irq = 1\t\t\t@ return from exception
""",
        """\t@ P105 aic1q24: like lab IRQ stub — force SPSR.I, skip C irq_enter/exit.
\tldr\tr0, =apple_aic1_early_irq_escape
\tldrb\tr0, [r0]
\tcmp\tr0, #0
\tbeq\t1001f
\tldr\tr5, [sp, #S_PSR]
\torr\tr5, r5, #PSR_I_BIT
\tsvc_exit r5, irq = 1
1001:
\tirq_handler from_user=0

#ifdef CONFIG_PREEMPTION
\tldr\tr8, [tsk, #TI_PREEMPT]\t\t@ get preempt count
\tldr\tr0, [tsk, #TI_FLAGS]\t\t@ get flags
\tteq\tr8, #0\t\t\t\t@ if preempt count != 0
\tmovne\tr0, #0\t\t\t\t@ force flags to 0
\ttst\tr0, #_TIF_NEED_RESCHED
\tblne\tsvc_preempt
#endif

\tldr\tr5, [sp, #S_PSR]
\tsvc_exit r5, irq = 1\t\t\t@ return from exception
""",
        "entry-armv: early IRQ escape from pristine __irq_svc",
    ),
    (
        "init/main.c",
        """\t\tp105_fb_dbg("init_go");
\t\tapple_aic1_quiesce();
\t\tapple_aic1_hw_quiesce();
\t\tapple_aic1_early_irq_escape = false;
\t\tearly_boot_irqs_disabled = false;
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("irq_thaw");
\t}
#endif

\tkernel_init_freeable();
""",
        """\t\tp105_fb_dbg("init_go");
\t\tapple_aic1_quiesce();
\t\tapple_aic1_hw_quiesce_quiet();
\t\tearly_boot_irqs_disabled = false;
\t\tp105_fb_dbg("irq_thaw");
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("thaw_on");
\t}
#endif

\tkernel_init_freeable();
""",
        "main.c: thaw keeps escape through enable, quiet P0",
    ),
    (
        "init/main.c",
        """\t\tp105_fb_dbg("init_go");
\t\tapple_aic1_quiesce();
\t\tapple_aic1_hw_quiesce_quiet();
\t\tearly_boot_irqs_disabled = false;
\t\tp105_fb_dbg("irq_thaw");
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("thaw_on");
\t}
#endif

\tkernel_init_freeable();
""",
        """\t\tvoid apple_aic1_release_escape(void);

\t\tp105_fb_dbg("init_go");
\t\tapple_aic1_quiesce();
\t\tapple_aic1_hw_quiesce_quiet();
\t\tearly_boot_irqs_disabled = false;
\t\tp105_fb_dbg("irq_thaw");
\t\tlocal_irq_enable();
\t\tp105_fb_dbg("thaw_on");
\t\tapple_aic1_release_escape();
\t\tp105_fb_dbg("esc_off");
\t}
#endif

\tkernel_init_freeable();
""",
        "main.c: release escape after thaw_on (lab I0 HOLD)",
    ),
    (
        "arch/arm/mm/alignment.c",
        """\tif ((!LDST_P_BIT(instr) && LDST_W_BIT(instr)) || user_mode(regs))
\t\tgoto trans;
""",
        """\tif ((!LDST_P_BIT(instr) && LDST_W_BIT(instr)) || user_mode(regs) ||
\t    addr < TASK_SIZE)
\t\tgoto trans;
""",
        "alignment: uaccess path for kernel get_user + PAN (P105 init oops)",
    ),
    (
        "arch/arm/mm/alignment.c",
        "\tif (user_mode(regs))\n\t\tgoto user;\n\n\tif (LDST_L_BIT(instr)) {\n\t\tunsigned long val;\n\t\tget16_unaligned_check(val, addr);\n",
        "\tif (user_mode(regs) || addr < TASK_SIZE)\n\t\tgoto user;\n\n\tif (LDST_L_BIT(instr)) {\n\t\tunsigned long val;\n\t\tget16_unaligned_check(val, addr);\n",
        "alignment: ldrh user-addr under PAN",
    ),
    (
        "arch/arm/mm/alignment.c",
        "\tai_dword += 1;\n\n\tif (user_mode(regs))\n\t\tgoto user;\n",
        "\tai_dword += 1;\n\n\tif (user_mode(regs) || addr < TASK_SIZE)\n\t\tgoto user;\n",
        "alignment: ldrd user-addr under PAN",
    ),
    (
        "arch/arm/mm/alignment.c",
        "\tif (user_mode(regs)) {\n\t\tunsigned int __ua_flags = uaccess_save_and_enable();\n\t\tfor (regbits = REGMASK_BITS(instr), rd = 0; regbits;\n",
        "\tif (user_mode(regs) || addr < TASK_SIZE) {\n\t\tunsigned int __ua_flags = uaccess_save_and_enable();\n\t\tfor (regbits = REGMASK_BITS(instr), rd = 0; regbits;\n",
        "alignment: ldmstm user-addr under PAN",
    ),
    (
        "init/main.c",
        """#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg(\"idle\");
#endif
\tschedule_preempt_disabled();
\t/* Call into cpu_idle with preempt disabled */
#ifdef CONFIG_ARCH_APPLE_S5L
\tp105_fb_dbg(\"cpu_idle\");
#endif
\tcpu_startup_entry(CPUHP_ONLINE);
""",
        """\tschedule_preempt_disabled();
\t/* Call into cpu_idle with preempt disabled */
\tcpu_startup_entry(CPUHP_ONLINE);
""",
        "main.c: no FB dbg after schedule (phys map gone / free_initmem)",
    ),
    (
        "init/main.c",
        """\texit_boot_config();
\tfree_initmem();
\tmark_readonly();
""",
        """\texit_boot_config();
#ifdef CONFIG_ARCH_APPLE_S5L
\t{
\t\tvoid p105_fb_dbg_shutdown(void);

\t\tp105_fb_dbg_shutdown();
\t}
#endif
\tfree_initmem();
\tmark_readonly();
""",
        "main.c: shutdown FB dbg before free_initmem",
    ),
    (
        "drivers/tty/serial/samsung_tty.c",
        """\tret = s3c24xx_serial_enable_baudclk(ourport);
\tif (ret)
\t\tpr_warn(\"uart: failed to enable baudclk\\n\");

\t/* Keep all interrupts masked and cleared */
""",
        """\tret = s3c24xx_serial_enable_baudclk(ourport);
\tif (ret)
\t\tpr_warn(\"uart: failed to enable baudclk\\n\");
\telse if (ourport->baudclk_rate)
\t\tport->uartclk = ourport->baudclk_rate;

\t/* Keep all interrupts masked and cleared */
""",
        "samsung_tty: set uartclk from baudclk (base baud 0 / console open)",
    ),
]


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def read(path: str) -> str:
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def write(path: str, text: str) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def apply_replacements(tree: str) -> int:
    changed = 0
    for rel, old, new, desc in REPLACEMENTS:
        path = os.path.join(tree, rel)
        if not os.path.exists(path):
            fail(f"{rel} not found in {tree}")
        text = read(path)
        if new in text:
            continue
        if old not in text:
            continue
        write(path, text.replace(old, new, 1))
        changed += 1
        print(f"  patched {rel} ({desc})")
    return changed


def verify(tree: str) -> None:
    main = read(os.path.join(tree, "init/main.c"))
    if "rest_clone" in main:
        fail("init/main.c still contains rest_clone")
    if "pre_spawn" not in main:
        fail("init/main.c missing pre_spawn (boot hacks not applied)")
    if "apple_s5l_rest_spawn_init" not in main:
        fail("init/main.c missing apple_s5l_rest_spawn_init")
    apple = os.path.join(tree, "arch/arm/mach-apple/apple.c")
    if os.path.exists(apple) and "aic1q34" not in read(apple):
        fail("arch/arm/mach-apple/apple.c missing aic1q34 stamp")
    if "apple_aic1_arm_cpu0" not in main:
        fail("init/main.c missing apple_aic1_arm_cpu0")
    if "apple_aic1_enable();\n\t\tp105_fb_dbg(\"irq_try\")" in main:
        fail("init/main.c still enables AIC before irq_try")
    if "irq_frz" not in main:
        fail("init/main.c missing irq_frz (must re-freeze after irq_arm)")
    if os.path.exists(apple) and "cbar_skip" not in read(apple):
        fail("arch/arm/mach-apple/apple.c missing cbar_skip (no CBAR MMIO)")
    if os.path.exists(apple) and "mio_cbar" in read(apple):
        fail("arch/arm/mach-apple/apple.c still maps CBAR (hangs)")
    if os.path.exists(apple) and "P105_AIC_EVENT_CPU0" not in read(apple):
        fail("apple.c missing EVENT_CPU0 0x5004 drain")
    if os.path.exists(apple) and "irq_ok" not in read(apple):
        fail("arch/arm/mach-apple/apple.c missing irq_ok (must not re-enable at irq_pre)")
    if os.path.exists(apple) and 'p105_fb_dbg("irq_pre")' in read(apple):
        fail("arch/arm/mach-apple/apple.c still has irq_pre (2nd enable storm)")
    if os.path.exists(apple) and "apple_spawn_on_alt_stack" in read(apple) and \
       "pid = apple_spawn_on_alt_stack" in read(apple):
        fail("apple.c still spawns on alt stack (kc_woke hang)")
    entry = os.path.join(tree, "arch/arm/kernel/entry-armv.S")
    if os.path.exists(entry) and "apple_aic1_early_irq_escape" not in read(entry):
        fail("entry-armv.S missing early IRQ asm escape (aic1q24)")
    boot_h = os.path.join(tree, "arch/arm/include/asm/apple_boot.h")
    if os.path.exists(boot_h) and "spin_lock_irq" in read(boot_h):
        fail("apple_boot.h still uses spin_lock_irq (cp_sig unlock storms)")
    fork = os.path.join(tree, "kernel/fork.c")
    if os.path.exists(fork) and "umt_in" not in read(fork):
        fail("kernel/fork.c missing umt_in (sync fork.c boot hacks)")
    if os.path.exists(fork) and "kc_woke" not in read(fork):
        fail("kernel/fork.c missing kc_woke")
    if os.path.exists(fork) and "apple_fork_fb_left" not in read(fork):
        fail("kernel/fork.c missing apple_fork_fb budget (fork FB flood)")
    if "kt_done" not in main:
        fail("init/main.c missing kt_done")
    if "init_run" not in main:
        fail("init/main.c missing init_run")
    if "irq_thaw" not in main:
        fail("init/main.c missing irq_thaw (unfreeze before initcalls)")
    if "thaw_on" not in main:
        fail("init/main.c missing thaw_on")
    if "esc_off" not in main:
        fail("init/main.c missing esc_off (release escape after thaw)")
    if "apple_aic1_release_escape" not in main:
        fail("init/main.c missing apple_aic1_release_escape")
    if "apple_aic1_early_irq_escape = false" in main and "irq_thaw" in main:
        thaw = main.split("irq_thaw")[0].split("init_go")[-1]
        if "apple_aic1_early_irq_escape = false" in thaw:
            fail("init/main.c clears early_irq_escape before thaw enable (storms)")
    aic1 = os.path.join(tree, "drivers/irqchip/irq-apple-aic1.c")
    if os.path.exists(aic1) and "AIC1_CPU_EVENT" not in read(aic1):
        fail("irq-apple-aic1.c missing per-CPU EVENT @0x5004")
    if os.path.exists(aic1) and "AIC1_IPI_SEND" not in read(aic1):
        fail("irq-apple-aic1.c missing IPI_SEND (SMP)")
    if os.path.exists(aic1) and "aic1_init_smp" not in read(aic1):
        fail("irq-apple-aic1.c missing aic1_init_smp")
    platsmp = os.path.join(tree, "arch/arm/mach-apple/platsmp.c")
    if os.path.exists(platsmp) and "apple,pmgr-core" not in read(platsmp):
        fail("platsmp.c missing apple,pmgr-core enable-method")
    if os.path.exists(apple) and "hw_quiesce_quiet" not in read(apple):
        fail("apple.c missing hw_quiesce_quiet")
    align = os.path.join(tree, "arch/arm/mm/alignment.c")
    if os.path.exists(align) and "addr < TASK_SIZE" not in read(align):
        fail("alignment.c missing addr<TASK_SIZE uaccess fix (PAN init oops)")
    fbdbg = os.path.join(tree, "arch/arm/mach-apple/p105_fb_dbg.c")
    if os.path.exists(fbdbg):
        fb = read(fbdbg)
        if "void __init p105_fb_dbg(" in fb:
            fail("p105_fb_dbg still __init (rest_init/cpu_idle after free_initmem)")
        if "p105_fb_dbg_shutdown" not in fb:
            fail("p105_fb_dbg.c missing p105_fb_dbg_shutdown")
    if "p105_fb_dbg(\"cpu_idle\")" in main:
        fail("init/main.c still stamps cpu_idle after schedule")
    if "p105_fb_dbg_shutdown" not in main:
        fail("init/main.c missing p105_fb_dbg_shutdown before free_initmem")
    samsung = os.path.join(tree, "drivers/tty/serial/samsung_tty.c")
    if os.path.exists(samsung):
        s = read(samsung)
        if "else if (ourport->baudclk_rate)" not in s or \
           "port->uartclk = ourport->baudclk_rate" not in s:
            fail("samsung_tty.c missing uartclk from baudclk_rate")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=os.path.join(ROOT, "build", "linux"))
    args = ap.parse_args()
    tree = os.path.abspath(args.tree)
    if not os.path.isfile(os.path.join(tree, "Makefile")):
        fail(f"{tree} is not a kernel tree")
    print(f"Applying P105 boot hacks in {tree}")
    n = apply_replacements(tree)
    verify(tree)
    print(f"{n} file(s) patched" if n else "boot hacks already up to date")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

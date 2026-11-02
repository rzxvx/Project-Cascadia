# SPDX-License-Identifier: MIT
"""G5.1 (a13/docs/research/gpu.md): one compute job through the A13's GPU
firmware, built by hand from m1n1's firmware structures (V13_3, G12).

m1n1's Python driver only submits render work (agx/render.py); this follows
its TA path and drm/asahi's compute.rs for the firmware side (WorkCommandCP
on a CL queue, microsequence StartCompute / timestamps / wait /
FinalizeCompute) and Mesa's genxml (src/asahi/genxml/cmdbuf.xml) for the
command stream the CDM reads:

    CDM Launch Word 0/1 -> USC words (Shared, Shader, Registers,
    No preshader) -> the shader; Global size, Local size, Barrier,
    Stream Terminate.

The shader is G12 machine code (a13/tools/gpu/isa/metal-isa.sh): the empty
kernel is 08 01 (G12's end of shader; G13's stop is 88 00).
"""
import struct
import time

from m1n1.constructutils import Ver
from m1n1.fw.agx.cmdqueue import (WorkCommandCP, EventControl, JobList, Flag,
                                  GPUContextData)
from m1n1.fw.agx.microsequence import (StartComputeCmd, FinalizeComputeCmd,
                                       ComputeInfo, ComputeInfo2, EncoderParams,
                                       JobMeta, TimestampCmd, WaitForInterruptCmd,
                                       TimeStamp, TimeStampPointers, Attachment,
                                       StampCounter, WrappedPointer)
from m1n1.agx.context import GPUContext, GPUWorkQueue, GPUMicroSequence
from construct import Int64ul, Int32ul


class GPUComputeWorkQueue(GPUWorkQueue):
    TYPE = 2    # RunCmdQueue queue_type: TA 0, 3D 1, CP 2


PIPELINE_BASE = 0x1100000000
EMPTY_SHADER = bytes.fromhex("0801")


def usc_words(code_off, nr_gprs=8):
    # Mesa agx_build_pipeline for a compute shader without uniforms,
    # textures, samplers or preamble (cmdbuf.xml "USC ...")
    w = b""
    # Shared: layout Vertex/compute (0x24), 64 KiB per threadgroup (the
    # all-groups encoding 0), no shared memory
    w += struct.pack("<I", 0x4d | (0x24 << 10))
    # Shader: unk_2 = 3 (Mesa), code = offset from the pipeline base
    w += struct.pack("<HI", 0x0d | (3 << 10), code_off)
    # Registers: count in groups of 8
    w += struct.pack("<I", 0x8d | (((nr_gprs + 7) // 8) << 8))
    # No preshader
    w += struct.pack("<H", 0x88)
    return w


def cdm_stream(usc_off, grid=(1, 1, 1), local=(1, 1, 1), barrier=True):
    # Launch word 0: uniform / texture / preshader register counts at their
    # minimum (one group each: 0 is not representable), sampler states 0,
    # mode Direct, block type Launch
    w0 = (1 << 1) | (1 << 4) | (1 << 12)
    s = struct.pack("<II", w0, usc_off & ~0x3f)
    s += struct.pack("<III", *grid)
    s += struct.pack("<III", *local)
    if barrier:
        # Mesa agx_cdm_barrier: bits 0-19, block type Barrier (3)
        s += struct.pack("<I", (3 << 29) | 0xfffff)
    end = len(s)
    # Stream Terminate (block type 2), 8 bytes
    s += struct.pack("<II", 2 << 29, 0)
    return s, end


class ComputeJob:
    def __init__(self, agx, ctx_id=3, queue=0):
        self.agx = agx
        self.queue = queue
        self.ctx = ctx = GPUContext(agx)
        ctx.bind(ctx_id)
        self.ctx_id = ctx_id

        self.scheduler_context = agx.kobj.new(GPUContextData).push()
        self.job_list = agx.kshared.new(JobList)
        self.job_list.first_job = 0
        self.job_list.last_head = self.job_list._addr
        self.job_list.unkptr_10 = 0
        self.job_list.push()

        self.wq = GPUComputeWorkQueue(agx, self.scheduler_context, self.job_list)
        self.wq.info.uuid = 0xc00000
        self.wq.info.push()

        self.stamp_value = 0xc0000000
        self.stamp1 = agx.kshared.new(StampCounter, name="CP stamp 1")
        self.stamp1.value = self.stamp_value
        self.stamp1.push()
        self.stamp2 = agx.kobj.new(StampCounter, name="CP stamp 2")
        self.stamp2.value = self.stamp_value
        self.stamp2.push()

        self.event_control = agx.kobj.new(EventControl)
        self.event_control.event_count = agx.kobj.new(Int32ul, "event_count")
        self.event_control.event_count.val = 0
        self.event_control.event_count.push()
        self.event_control.submission_id = 0
        self.event_control.cur_count = 0
        self.event_control.unk_10 = 0x50
        self.event_control.push()

        self.ev = agx.event_mgr.allocate_event()

    def submit(self, shader=EMPTY_SHADER, grid=(1, 1, 1), local=(1, 1, 1),
               nr_gprs=8, barrier=True, uuid=0xc0de0001):
        agx, ctx = self.agx, self.ctx

        # shader and USC words in the pipeline region (32-bit offsets from
        # PIPELINE_BASE; the CDM's pipeline pointer is 64-byte aligned)
        code = ctx.pobj.new_buf(0x100, "G5 shader", track=False)
        code.val = shader + bytes(0x100 - len(shader))
        code.push()
        code_off = code._addr - PIPELINE_BASE
        usc = ctx.pobj.new_buf(0x40, "G5 USC words", track=False)
        uw = usc_words(code_off, nr_gprs)
        usc.val = uw + bytes(0x40 - len(uw))
        usc.push()
        usc_off = usc._addr - PIPELINE_BASE
        assert usc_off & 0x3f == 0, hex(usc._addr)

        cdm, end = cdm_stream(usc_off, grid, local, barrier)
        enc = ctx.uobj.new_buf(0x100, "G5 CDM stream", track=False)
        enc.val = cdm + bytes(0x100 - len(cdm))
        enc.push()
        self.encoder = enc
        agx.log(f"[G5] shader @ {code._addr:#x} ({shader.hex()}), USC @ "
                f"{usc._addr:#x} ({uw.hex()}), CDM @ {enc._addr:#x} "
                f"({cdm.hex()})")

        # drm/asahi: preempt buffer, its four 8-byte slots after
        # compute_preempt1_size (0x7f80 on t8103)
        preempt = ctx.uobj.new_buf(0x7fa0, "G5 preempt", track=False).push()

        def ts(alloc, name):
            t = alloc.new(Int64ul, name=name)
            t.val = 0
            t.push()
            return t
        self.ts_start = ts(agx.kshared, "CP ts start")
        self.ts_end = ts(agx.kshared, "CP ts end")
        self.uts_start = ts(agx.ktimestamp, "CP user ts start")
        self.uts_end = ts(agx.ktimestamp, "CP user ts end")

        self.ev.rearm()
        self.stamp_value += 0x100
        self.event_control.event_count.val += 1
        self.event_control.event_count.push()

        wc = agx.cmdbuf.new(WorkCommandCP, track=False, align=0x20)
        self.wc = wc
        wc.counter = 1
        wc.unk_4 = 0
        wc.context_id = self.ctx_id
        wc.event_control = self.event_control
        wc.unk_2c = 0
        wc.unk_buf = bytes(0x50)

        ci = ComputeInfo()
        ci.iogpu_deflake_1 = preempt._addr
        ci.encoder = enc._addr
        ci.iogpu_deflake_2 = preempt._addr + 0x7f80
        ci.iogpu_deflake_3 = preempt._addr + 0x7f88
        ci.iogpu_deflake_4 = preempt._addr + 0x7f90
        ci.iogpu_deflake_5 = preempt._addr + 0x7f98
        ci.pipeline_base = PIPELINE_BASE
        ci.unk_38 = 0x8c60
        ci.helper_program = 0
        ci.unk_44 = 0
        ci.helper_arg = 0
        ci.helper_cfg = 0
        ci.unk_54 = 0
        ci.unk_58 = 1
        ci.unk_5c = 0
        ci.iogpu_unk_40 = 0
        wc.compute_info = ci

        wc.registers_addr = 0
        wc.register_count = 0
        wc.registers_length = 0
        wc.unk_pad = bytes(0x24)

        ci2 = ComputeInfo2()
        ci2.unk_0 = bytes(0x24)
        ci2.iogpu_deflake_1 = preempt._addr
        ci2.encoder_end = enc._addr + end
        ci2.unk_34 = bytes(0x20)
        ci2.unk_g14x = 0
        ci2.unk_58 = 0
        wc.compute_info2 = ci2

        ep = EncoderParams()
        ep.unk_0 = 0
        ep.unk_4 = 0
        ep.unk_8 = 0
        ep.encoder_id = 0x1
        ep.unk_10 = 0
        ep.iogpu_compute_unk44 = 0xffffffff
        ep.seq_buffer = 0
        ep.unk_1c = 0
        wc.encoder_params = ep

        jm = JobMeta()
        jm.unk_0 = 0
        jm.stamp_addr = WrappedPointer(self.stamp1._addr)
        jm.fw_stamp_addr = WrappedPointer(self.stamp2._addr)
        jm.stamp_value = self.stamp_value
        jm.stamp_slot = self.ev.id
        jm.evctl_index = 0
        jm.unk_20 = 0
        jm.uuid = uuid
        jm.queue_cmd_count = 0
        wc.job_meta = jm

        wc.ts1 = TimeStamp(0)
        wc.ts_pointers = TimeStampPointers(self.ts_start._addr, self.ts_end._addr)
        wc.user_ts_pointers = TimeStampPointers(self.uts_start._addr, self.uts_end._addr)
        wc.client_sequence = 1
        wc.unk_ts2 = TimeStamp()
        wc.unk_ts = TimeStamp()
        wc.unk_2e1 = bytes(0x1c)
        wc.unk_flag = Flag()
        wc.unk_pad2 = bytes(0x10)
        wc.pad_2d9 = bytes(7)
        # placeholders until the microsequence exists
        wc.microsequence_ptr = 0
        wc.microsequence_size = 0
        wc.set_addr()

        stats = agx.initdata.regionB.stats_cp
        stats_addr = getattr(stats, "_addr", stats)

        ms = GPUMicroSequence(agx)
        st = StartComputeCmd()
        st.unk_buf_addr = wc.addrof("unk_buf")
        st.computeinfo_addr = wc.compute_info._addr
        st.stats_ptr = stats_addr
        st.cmdqueue_ptr = self.wq.info._addr
        st.context_id = self.ctx_id
        st.unk_28 = 1
        st.counter1 = 0
        st.counter2 = 0
        st.unk_34 = 0
        st.unk_38 = 0
        st.computeinfo2_addr = wc.compute_info2._addr
        st.unk_44 = 0
        st.uuid = uuid
        st.attachments = [Attachment(0, 0, 0, 0) for i in range(16)]
        st.num_attachments = 0
        st.padding = bytes(4)
        st.unk_flag_addr = wc.addrof("unk_flag")
        st.counter = wc.counter
        st.event_ctrl_buf_addr = self.event_control.unk_buf._addr
        start_off = ms.append(st)

        def tscmd(unk_3):
            t = TimestampCmd()
            t.unk_1 = 0
            t.unk_2 = 0
            t.unk_3 = unk_3
            t.ts0_addr = wc.ts1._addr
            t.ts_pointers_addr = wc.ts_pointers._addr
            t.unk_addr = 0xdeadbeef
            t.cmdqueue_ptr = self.wq.info._addr
            t.user_ts_pointers_addr = wc.user_ts_pointers._addr
            t.unk_ts_addr = wc.unk_ts._addr
            t.uuid = uuid
            t.unk_30 = 0
            return t
        ms.append(tscmd(0x80))
        # TA waits with (1, 0, 0), 3D with (0, 1, 0): compute (0, 0, 1)
        ms.append(WaitForInterruptCmd(0, 0, 1))
        ms.append(tscmd(0))

        fin = FinalizeComputeCmd()
        fin.unkptr_4 = stats_addr
        fin.cmdqueue_ptr = self.wq.info._addr
        fin.context_id = self.ctx_id
        fin.computeinfo2_addr = wc.compute_info2._addr
        fin.unk_24 = 0
        fin.uuid = uuid
        fin.stamp = self.stamp2._addr
        fin.stamp_value = self.stamp_value
        for f in ("unk_38", "unk_3c", "unk_40", "unk_44", "unk_48", "unk_4c",
                  "unk_50", "unk_54", "unk_58", "unk_60"):
            setattr(fin, f, 0)
        fin.restart_branch_offset = start_off - ms.off
        fin.unk_64 = bytes(0xd)
        fin.unkptr_71 = wc.addrof("unk_flag")
        ms.append(fin)
        ms.finalize()
        self.ms = ms

        wc.microsequence_ptr = ms.obj._addr
        wc.microsequence_size = ms.size
        agx.log(f"[G5] WorkCommandCP @ {wc._addr:#x}, microsequence "
                f"{ms.size:#x} bytes @ {ms.obj._addr:#x}, event {self.ev.id}, "
                f"stamp {self.stamp_value:#x}")
        self.wq.submit(wc)

    def crash_check(self):
        # the firmware may crash without telling the host (G5.1 run 1: a
        # NULL dereference 0.3 ms after the CL doorbell, no crash message)
        crash = self.agx.asc.crash
        buf = getattr(crash, "iobuffer", None)
        if buf is None or self.agx.p.read32(buf) != 0x434c4845:  # "EHLC"
            return False
        self.agx.log("[G5] the firmware crashed (crash log in its buffer):")
        from m1n1.fw.asc.crash import CrashLogParser
        data = self.agx.u.iface.readmem(buf, 0x4000)
        open("g5-crash.bin", "wb").write(data)
        try:
            CrashLogParser(data, self.agx.asc).dump()
        except Exception as e:
            self.agx.log(f"[G5] crash log parse stopped: {e!r}")
        return True

    def run(self):
        self.agx.log("[G5] RunCmdQueue CL")
        self.agx.ch.queue[self.queue].q_CL.run(self.wq, self.ev.id)

    def state(self, when):
        ch = self.agx.ch.queue[self.queue].q_CL
        pm = self.wq.pmap
        self.agx.log(f"[G5] {when}: CL_{self.queue} channel read {ch.state.READ_PTR.val} "
                     f"write {ch.state.WRITE_PTR.val}; queue GPU_RPTR "
                     f"{pm.GPU_RPTR.val} GPU_DONEPTR {pm.GPU_DONEPTR.val} "
                     f"CPU_WPTR {pm.CPU_WPTR.val}")

    def _wait(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.agx.work()
            self.stamp1.pull()
            if self.stamp1.value >= self.stamp_value or self.ev.fired:
                return True
        return False

    def wait(self, timeout=5.0, kick=True, soft_crash=True):
        self.state("after RunCmdQueue")
        if not self._wait(timeout) and kick:
            # drm/asahi rings doorbell 0x10 (kick firmware) after a submission
            self.state(f"after {timeout} s")
            self.agx.log("[G5] kick_firmware (doorbell 0x10)")
            self.agx.kick_firmware()
            self._wait(3.0)
            self.state("after kick")
        self.stamp1.pull()
        self.stamp2.pull()
        self.ts_start.pull()
        self.ts_end.pull()
        done = self.stamp1.value >= self.stamp_value
        self.agx.log(f"[G5] {'DONE' if done else 'NOT DONE'}: stamp1 "
                     f"{self.stamp1.value:#x} stamp2 {self.stamp2.value:#x} "
                     f"(want {self.stamp_value:#x}), event fired "
                     f"{self.ev.fired}, ts {self.ts_start.val:#x} -> "
                     f"{self.ts_end.val:#x}, queue done ptr "
                     f"{self.wq.pmap.GPU_DONEPTR.val}")
        if not done and self.crash_check():
            return done
        if not done and soft_crash:
            # the firmware dumps its tasks (what they wait on) and stops
            self.agx.log("[G5] soft crash for a task dump")
            self.agx.asc.crash.crash_soft()
            end = time.time() + 3
            while time.time() < end:
                try:
                    self.agx.work()
                except Exception as e:
                    self.agx.log(f"[G5] after soft crash: {e!r}")
                    break
        return done


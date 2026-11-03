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
from m1n1.constructutils import ConstructClass, ROPointer
from construct import (Int64ul, Int32ul, Int32sl, Int16ul, Int8ul, Struct, Const,
                       Hex, HexDump, Bytes, Tell, this)


class GPUComputeWorkQueue(GPUWorkQueue):
    TYPE = 2    # RunCmdQueue queue_type: TA 0, 3D 1, CP 2


class G12EncoderParams(ConstructClass):
    # drm/asahi EncoderParams without the sampler fields (0x18, not 0x28)
    subcon = Struct(
        "unk_0" / Int32ul,
        "unk_4" / Int32ul,
        "unk_8" / Int32ul,
        "encoder_id" / Int32ul,
        "unk_10" / Int32ul,
        "iogpu_compute_unk44" / Int32ul,
    )


class G12WorkCommandCP(ConstructClass):
    """The G12 firmware's compute work command (0x2e0 bytes: armfw_g12p
    flushes 0x2e0 for type 3, fw 0x20150), from what it reads (gpu.md, G5.1):
    V13_3's head (counter at +4, context_id +0x10, event_control +0x14,
    microsequence +0x1f0 / +0x1f8), JobMeta at +0x274 (no_preemption byte
    +0x277, stamp +0x278, stamp_value +0x288, stamp_slot +0x28c, evctl_index
    +0x290, flush +0x294, uuid +0x298), timestamps at +0x2a0.., so 0x10 less
    between ComputeInfo2 and JobMeta than V13_3: taken here as EncoderParams
    without its sampler fields."""
    subcon = Struct(
        "addr" / Tell,
        "magic" / Const(0x3, Hex(Int32ul)),
        "counter" / Int64ul,
        "unk_4" / Hex(Int32ul),
        "context_id" / Hex(Int32ul),
        "event_control_addr" / Hex(Int64ul),
        "event_control" / ROPointer(this.event_control_addr, EventControl),
        "unk_2c" / Int32ul,
        "unk_buf" / HexDump(Bytes(0x50)),
        "compute_info" / ComputeInfo,
        "registers_addr" / Int64ul,
        "register_count" / Int16ul,
        "registers_length" / Int16ul,
        "unk_pad" / HexDump(Bytes(0x24)),
        "microsequence_ptr" / Hex(Int64ul),
        "microsequence_size" / Hex(Int32ul),
        "compute_info2" / ComputeInfo2,
        "encoder_params" / G12EncoderParams,
        "job_meta" / JobMeta,
        "ts1" / TimeStamp,
        "ts_pointers" / TimeStampPointers,
        "user_ts_pointers" / TimeStampPointers,
        "client_sequence" / Int8ul,
        "pad_2c9" / HexDump(Bytes(3)),
        "unk_flag" / Flag,
        # Timestamp ops and StartCompute only point at unk_ts / unk_flag,
        # so where they sit in the tail is ours to choose
        "unk_ts" / TimeStamp,
        "pad_2d8" / HexDump(Bytes(8)),
    )


PIPELINE_BASE = 0x1100000000
EMPTY_SHADER = bytes.fromhex("0801")


def usc_words(code_off, uniforms=()):
    """The G12 USC control program, as iOS 16.4's AGXMetalG12 writes it
    (gpu.md, "iOS reference", a13/build/gpu/g12cap-2): it is itself G12
    code-like, ended by G12's 0x0108 as Mesa's G13 words end with G13's
    stop 0x88. uniforms: (start_halfs, size_halfs, gpu_addr) loads, tag
    0x03 with Mesa's Uniform field layout."""
    w = b""
    for start, size, addr in uniforms:
        w += struct.pack("<Q", 0x03 | (start << 8) | (size << 20) | ((addr >> 2) << 26))
    # shader: tag 0x1f, flags 80 00 90 as Apple's, code = offset from the
    # pipeline base
    w += bytes.fromhex("1f800090") + struct.pack("<I", code_off)
    w += bytes.fromhex("0000")
    w += bytes.fromhex("0801")
    return w


def cdm_stream(usc_off, threads=(1, 1, 1), local=(1, 1, 1), barrier=True):
    """CDM stream as AGXMetalG12 writes it: launch word 0 = 0x2, word 1 =
    USC offset | 4, the global size in threads (not groups), the local
    size, barrier 0x60000160 (Mesa's base bits 5, 6, 8), terminate."""
    s = struct.pack("<II", 0x2, (usc_off & ~0x3f) | 4)
    s += struct.pack("<III", *threads)
    s += struct.pack("<III", *local)
    if barrier:
        s += struct.pack("<I", 0x60000160)
    end = len(s)
    s += struct.pack("<II", 2 << 29, 0)
    return s, end


class G12FinalizeComputeCmd(ConstructClass):
    """0x6c bytes on G12 (Apple's microsequence, g12cap-3): V13_3's up to
    restart_branch_offset, then one byte and the unk_flag pointer
    (unaligned at +0x5d), not V13_3's unk_60 / unk_64[0xd] / unkptr_71."""
    subcon = Struct(
        "magic" / Const(0x2a, Int32ul),
        "unkptr_4" / Int64ul,
        "cmdqueue_ptr" / Int64ul,
        "context_id" / Int32ul,
        "computeinfo2_addr" / Int64ul,
        "unk_24" / Int32ul,
        "uuid" / Int32ul,
        "stamp" / Int64ul,
        "stamp_value" / Int32ul,
        "unk_38" / HexDump(Bytes(0x24)),
        "restart_branch_offset" / Int32sl,
        "unk_60" / Int8ul,
        "unkptr_61" / Int64ul,
        "pad_69" / HexDump(Bytes(7)),
    )


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
               uniforms=(), barrier=True, uuid=0xc0de0001):
        agx, ctx = self.agx, self.ctx

        # shader and USC words in the pipeline region (32-bit offsets from
        # PIPELINE_BASE; the CDM's pipeline pointer is 64-byte aligned)
        code = ctx.pobj.new_buf(0x100, "G5 shader", track=False)
        code.val = shader + bytes(0x100 - len(shader))
        code.push()
        code_off = code._addr - PIPELINE_BASE
        usc = ctx.pobj.new_buf(0x40, "G5 USC words", track=False)
        uw = usc_words(code_off, uniforms)
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

        # ComputeInfo +0x0: a per-submission scratch buffer (Apple's is
        # 0x8000, with the argument table inside it)
        preempt = ctx.uobj.new_buf(0x8000, "G5 scratch", track=False).push()

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

        wc = agx.cmdbuf.new(G12WorkCommandCP, track=False, align=0x20)
        self.wc = wc
        wc.counter = 0     # Apple: 0
        wc.unk_4 = 0
        wc.context_id = self.ctx_id
        wc.event_control = self.event_control
        wc.unk_2c = 0
        wc.unk_buf = bytes(0x50)

        # G12 ComputeInfo (Apple's, g12cap-3): only the six qwords the
        # firmware copies: +0x0 scratch, +0x8 CDM stream, +0x18 pipeline
        # base (M1's +0x30), the rest zero
        ci = ComputeInfo()
        ci.iogpu_deflake_1 = preempt._addr
        ci.encoder = enc._addr
        ci.iogpu_deflake_2 = 0
        ci.iogpu_deflake_3 = PIPELINE_BASE
        ci.iogpu_deflake_4 = 0
        ci.iogpu_deflake_5 = 0
        ci.pipeline_base = 0
        ci.unk_38 = 0
        ci.helper_program = 0
        ci.unk_44 = 0
        ci.helper_arg = 0
        ci.helper_cfg = 0
        ci.unk_54 = 0
        ci.unk_58 = 0
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

        ep = G12EncoderParams()
        ep.unk_0 = 0
        ep.unk_4 = 0
        ep.unk_8 = 0
        ep.encoder_id = 0x1
        ep.unk_10 = 0
        ep.iogpu_compute_unk44 = 0xffffffff
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
        wc.pad_2c9 = bytes(3)
        wc.unk_flag = Flag()
        wc.unk_ts = TimeStamp()
        wc.pad_2d8 = bytes(8)
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
        st.unk_28 = 0
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

        def tscmd(unk_3, end):
            t = TimestampCmd()
            t.unk_1 = 0
            t.unk_2 = 0
            t.unk_3 = unk_3
            t.ts0_addr = wc.ts1._addr
            t.ts_pointers_addr = wc.ts_pointers._addr
            # Apple: the start / end slot of ts_pointers
            t.unk_addr = wc.ts_pointers._addr + (8 if end else 0)
            t.cmdqueue_ptr = self.wq.info._addr
            t.user_ts_pointers_addr = wc.user_ts_pointers._addr
            t.unk_ts_addr = wc.unk_ts._addr
            t.uuid = uuid
            t.unk_30 = 0
            return t
        ms.append(tscmd(0x80, False))
        # Apple's compute wait on G12: 01 00 80 00
        ms.append(WaitForInterruptCmd(0, 0x80, 0))
        ms.append(tscmd(0, True))

        fin = G12FinalizeComputeCmd()
        fin.unkptr_4 = stats_addr
        fin.cmdqueue_ptr = self.wq.info._addr
        fin.context_id = self.ctx_id
        fin.computeinfo2_addr = wc.compute_info2._addr
        fin.unk_24 = 0
        fin.uuid = uuid
        fin.stamp = self.stamp2._addr
        fin.stamp_value = self.stamp_value
        fin.unk_38 = bytes(0x24)
        fin.restart_branch_offset = start_off - ms.off
        fin.unk_60 = 0
        fin.unkptr_61 = wc.addrof("unk_flag")
        fin.pad_69 = bytes(7)
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


"""Compile and execute loader prologues in fake RAM; never boot a device."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE, KERNEL, BOTTOM = 0x80000000, 0x80008000, 0x80006000


class Stack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("arm-linux-gnueabihf-gcc", "arm-linux-gnueabihf-objcopy", "arm-linux-gnueabihf-nm"):
            if not shutil.which(tool):
                raise unittest.SkipTest("ARM compilation tools unavailable")
        try:
            import unicorn
        except ImportError:
            raise unittest.SkipTest("Unicorn unavailable")
        cls.uc = unicorn
        cls.tmp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.tmp.name)
        (cls.work / "staging-generated.h").write_text(
            "#define DTB_PHYS 0x82000000u\n#define DTB_SIZE 0x1000u\n")
        cls.elf, cls.binary = cls.compile("current")
        cls.symbols = cls.read_symbols(cls.elf)
        historical = cls.work / "historical.S"
        source = (ROOT / "pongo/linux_boot_start.S").read_text()
        assert "ldr\tsp, =__stack_top" in source
        historical.write_text(source.replace("ldr\tsp, =__stack_top", "ldr\tsp, =0x8000F000"))
        cls.old_elf, cls.old_binary = cls.compile("historical", asm=historical)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @classmethod
    def compile(cls, name, asm=None, extra=None, expect_success=True):
        elf, binary = cls.work / (name + ".elf"), cls.work / (name + ".bin")
        command = ["arm-linux-gnueabihf-gcc", "-static", "-nostdlib", "-nostartfiles",
                   "-O2", "-fno-builtin", "-ffreestanding", "-Werror",
                   "-I" + str(cls.work), "-I" + str(ROOT / "pongo"),
                   "-Wl,--build-id=none",
                   "-Wl,-T," + str(ROOT / "pongo/linux-boot.ld"), "-o", str(elf),
                   str(asm or ROOT / "pongo/linux_boot_start.S"),
                   str(ROOT / "pongo/linux_boot.c")]
        if extra:
            command.append(str(extra))
        result = subprocess.run(command, capture_output=True, text=True)
        if not expect_success:
            return result
        if result.returncode:
            raise AssertionError(result.stderr)
        subprocess.run(["arm-linux-gnueabihf-objcopy", "-O", "binary", str(elf), str(binary)], check=True)
        return elf, binary.read_bytes()

    @staticmethod
    def read_symbols(elf):
        rows = subprocess.check_output(["arm-linux-gnueabihf-nm", "-n", str(elf)], text=True)
        return {parts[2]: int(parts[0], 16) for row in rows.splitlines()
                if len(parts := row.split()) == 3}

    def prologue(self, binary, symbols, function, start_stack=None):
        from unicorn.arm_const import UC_ARM_REG_SP
        uc = self.uc.Uc(self.uc.UC_ARCH_ARM, self.uc.UC_MODE_ARM)
        uc.mem_map(BASE, 0x20000)
        uc.mem_write(BASE, binary)
        # Fake kernel canary and watchdog RAM: no hardware MMIO is accessed.
        uc.mem_write(KERNEL, bytes([0xA5]) * 0x10000)
        uc.mem_map(0x0F103000, 0x1000)
        if start_stack is not None:
            uc.reg_write(UC_ARM_REG_SP, start_stack)
        writes = []

        def memory(_uc, _access, address, size, value, _data):
            if BASE <= address < BASE + 0x20000:
                writes.append((address, size, value))

        def instruction(_uc, _address, _size, _data):
            if writes:
                _uc.emu_stop()

        uc.hook_add(self.uc.UC_HOOK_MEM_WRITE, memory)
        uc.hook_add(self.uc.UC_HOOK_CODE, instruction)
        uc.emu_start(symbols[function], BASE + 0x20000, count=100)
        self.assertTrue(writes, function)
        intact = bytes(uc.mem_read(KERNEL, 0x10000)) == bytes([0xA5]) * 0x10000
        return writes, intact

    def test_linker_reserves_stack_without_binary_payload(self):
        self.assertEqual(self.symbols["__stack_bottom"], BOTTOM)
        self.assertEqual(self.symbols["__stack_top"], KERNEL)
        self.assertLessEqual(self.symbols["__loader_end"], BOTTOM)
        self.assertEqual(len(self.binary), self.symbols["__loader_end"] - BASE)
        self.assertEqual(KERNEL % 8, 0)
        data = self.elf.read_bytes()
        # Inspect real ELF section types/sizes rather than trusting source text.
        shoff = struct.unpack_from("<I", data, 32)[0]
        shsize, count, strindex = struct.unpack_from("<HHH", data, 46)
        strhdr = struct.unpack_from("<10I", data, shoff + strindex * shsize)
        strings = data[strhdr[4]:strhdr[4] + strhdr[5]]
        stack = []
        for index in range(count):
            header = struct.unpack_from("<10I", data, shoff + index * shsize)
            name = strings[header[0]:].split(b"\0", 1)[0]
            if name == b".stack":
                stack.append(header)
        self.assertEqual(len(stack), 1)
        self.assertEqual((stack[0][1], stack[0][3], stack[0][5]), (8, BOTTOM, 8192))

    def test_actual_entry_and_cache_prologues_preserve_kernel(self):
        for function in ("_start", "v7_flush_dcache_all", "l2x0_flush_all"):
            with self.subTest(function=function):
                writes, intact = self.prologue(self.binary, self.symbols, function,
                                               None if function == "_start" else KERNEL)
                self.assertTrue(intact)
                self.assertTrue(all(BOTTOM <= address and address + size <= KERNEL
                                    for address, size, _ in writes))

    def test_historical_entry_overwrites_kernel_canary(self):
        writes, intact = self.prologue(self.old_binary, self.read_symbols(self.old_elf), "_start")
        self.assertFalse(intact)
        self.assertTrue(all(KERNEL <= address < BASE + 0xF000 for address, _, _ in writes))

    def test_loader_growth_link_fails_before_stack_overlap(self):
        extra = self.work / "oversize.S"
        extra.write_text('.section .text,"ax"\n.space 24576\n')
        result = self.compile("oversize", extra=extra, expect_success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("linux-boot loader overlaps reserved stack", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)

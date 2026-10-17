"""Compile isolated C protocol components with native Linux errno and fake types."""
from pathlib import Path
import subprocess
import tempfile


def native_stage(source, files, shared=False, sanitize=True):
    temp = tempfile.TemporaryDirectory(prefix="cascadia-experiment-")
    out = Path(temp.name)
    headers = out / "include/linux"
    headers.mkdir(parents=True)
    (headers / "types.h").write_text(
        "#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n"
        "#include <stdint.h>\ntypedef uint8_t u8; typedef uint16_t u16;\n"
        "typedef uint32_t u32; typedef uint64_t u64;\n")
    (headers / "errno.h").write_text("#pragma once\n#include_next <linux/errno.h>\n")
    (headers / "string.h").write_text("#pragma once\n#include <string.h>\n")
    binary = out / ("component.so" if shared else "component")
    flags = ["-shared", "-fPIC"] if shared else []
    if sanitize:
        flags += ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
    try:
        subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(out / "include"), "-I", str(source), *flags,
                        *[str(source / name) for name in files], "-o", str(binary)],
                       check=True)
    except BaseException:
        temp.cleanup()
        raise
    return temp, binary

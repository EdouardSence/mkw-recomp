"""GLSL to SPIR-V inside the calling process, through libshaderc (the library glslc is built on), loaded with ctypes.

Why: one glslc per program spends most of its time setting up glslang's built-in declarations again: 240 programs took
40.3 s that way and 11.7 s in one process, with byte-identical SPIR-V (11/11/2026). The player's package already
carries the library beside its glslc (conda-forge shaderc: lib/libshaderc.so.1, shaderc.dll); a system install has
libshaderc_shared. Without either, translate.py runs glslc as before.

Usage: comp = Shaderc.near(glslc_path); spv, err = comp.compile(src, "ps", "name.glsl")
"""

import ctypes
import ctypes.util
import os
from pathlib import Path

# shaderc.h shaderc_shader_kind (printed from the header: task and mesh come after the ray tracing kinds)
KIND = {"vs": 0, "ps": 1, "cs": 2, "task": 26, "mesh": 27}
VULKAN, VULKAN_1_3 = 0, (1 << 22) | (3 << 12)  # env.h
PERFORMANCE = 2  # glslc -O

_P = ctypes.c_void_p
_API = {
    "shaderc_compiler_initialize": (_P, []),
    "shaderc_compile_options_initialize": (_P, []),
    "shaderc_compile_options_set_target_env": (None, [_P, ctypes.c_int, ctypes.c_uint32]),
    "shaderc_compile_options_set_optimization_level": (None, [_P, ctypes.c_int]),
    "shaderc_compile_into_spv": (_P, [_P, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_char_p,
                                      ctypes.c_char_p, _P]),  # fmt: skip
    "shaderc_result_get_compilation_status": (ctypes.c_int, [_P]),
    "shaderc_result_get_length": (ctypes.c_size_t, [_P]),
    "shaderc_result_get_bytes": (_P, [_P]),
    "shaderc_result_get_error_message": (ctypes.c_char_p, [_P]),
    "shaderc_result_release": (None, [_P]),
}


def find(glslc: str) -> str | None:
    """The library that goes with this glslc: beside the package's, else the system's."""
    if os.sep in glslc or "/" in glslc:
        exe = Path(glslc)
        for lib in (exe.parent / "shaderc.dll", exe.parent.parent / "lib/libshaderc.so.1",
                    exe.parent.parent / "lib/libshaderc_shared.so.1"):  # fmt: skip
            if lib.is_file():
                return str(lib)
        return None  # a glslc of its own (a test's stand-in): no library of the system behind its back
    return ctypes.util.find_library("shaderc_shared")


class Shaderc:
    """One compiler and its options, for one process (a translate.py worker)."""

    def __init__(self, path: str) -> None:
        self.lib = ctypes.CDLL(path)
        for name, (res, args) in _API.items():
            fn = getattr(self.lib, name)
            fn.restype, fn.argtypes = res, args
        self.comp = self.lib.shaderc_compiler_initialize()
        self.opts = self.lib.shaderc_compile_options_initialize()
        self.lib.shaderc_compile_options_set_target_env(self.opts, VULKAN, VULKAN_1_3)
        self.lib.shaderc_compile_options_set_optimization_level(self.opts, PERFORMANCE)

    @classmethod
    def near(cls, glslc: str) -> "Shaderc | None":
        path = find(glslc)
        return cls(path) if path else None

    def compile(self, src: str, stage: str, name: str) -> tuple[bytes, str]:
        """(SPIR-V, "") or (b"", the error text as glslc prints it)."""
        data = src.encode()
        r = self.lib.shaderc_compile_into_spv(self.comp, data, len(data), KIND[stage], name.encode(), b"main", self.opts)
        try:
            if self.lib.shaderc_result_get_compilation_status(r):
                return b"", (self.lib.shaderc_result_get_error_message(r) or b"").decode(errors="replace")
            return ctypes.string_at(self.lib.shaderc_result_get_bytes(r), self.lib.shaderc_result_get_length(r)), ""
        finally:
            self.lib.shaderc_result_release(r)

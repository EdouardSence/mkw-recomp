"""SM86 program -> GLSL 4.6. The translator lives in core.py (glsl.core.Builder and translate()); the instruction groups
are mixins in alu.py, mem.py, tex.py and thread.py, the declarations before main() in head.py."""

from glsl.common import Unsupported
from glsl.core import Builder, compute_shape, translate

__all__ = ["Builder", "Unsupported", "compute_shape", "translate"]

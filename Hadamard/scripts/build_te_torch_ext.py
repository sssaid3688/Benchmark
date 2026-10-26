#!/usr/bin/env python3
"""Standalone rebuild of transformer_engine_torch pybind extension.

Mirrors build_tools/pytorch.py::setup_pytorch_extension but skips the
CMake common-lib build (already built incrementally with doublewarp source).
Compiles only transformer_engine/pytorch/csrc/*.cpp against the host torch.
"""
import ctypes
import os
from pathlib import Path

os.environ["NVTE_WITH_NCCL_EP"] = "0"   # match cmake cache (stubs, OFF)
os.environ.setdefault("MAX_JOBS", "32")

TE_ROOT = Path("/workspace/hadamard_base/TransformerEngine")
CSRC = TE_ROOT / "transformer_engine" / "pytorch" / "csrc"
BUILD_DIR = Path("/tmp/te_torch_build")
BUILD_DIR.mkdir(parents=True, exist_ok=True)

# Pre-load core lib with RTLD_GLOBAL so nvte_* symbols resolve at import time
ctypes.CDLL(str(TE_ROOT / "transformer_engine" / "libtransformer_engine.so"),
            mode=ctypes.RTLD_GLOBAL | os.RTLD_LAZY)

from torch.utils.cpp_extension import load  # noqa: E402

sources = sorted(str(p) for p in CSRC.rglob("*.cpp"))
print(f"[build] {len(sources)} cpp sources")

include_dirs = [
    "/usr/local/cuda/include",
    str(TE_ROOT / "transformer_engine"),
    str(TE_ROOT / "transformer_engine" / "common"),
    str(TE_ROOT / "transformer_engine" / "common" / "include"),
    str(CSRC),
]
cxx_flags = ["-O3", "-fvisibility=hidden", "-g0"]

mod = load(
    name="transformer_engine_torch",
    sources=sources,
    extra_include_paths=include_dirs,
    extra_cflags=cxx_flags,
    build_directory=str(BUILD_DIR),
    verbose=True,
)
print("[build] import OK:", mod.__file__)

# Copy built .so into TE source tree (replaces container-built one)
import glob
import shutil
built = glob.glob(str(BUILD_DIR / "transformer_engine_torch*.so"))
assert built, "built .so not found"
dst = TE_ROOT / "transformer_engine"
for b in built:
    shutil.copy2(b, dst / Path(b).name)
    print("[build] deployed:", dst / Path(b).name)

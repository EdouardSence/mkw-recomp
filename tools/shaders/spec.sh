#!/usr/bin/env bash
# Build the SM86 instruction spec the shader decoder reads (build/sm86/instructions.json).
#
# Why: the spec is NVIDIA's own description of the encoding, printed by nvdisasm at run time; it is generated on this
# machine and never versioned. DocumentSASS intercepts it and nvsass-disassembler parses it (both MIT, pinned below).
# nvdisasm needs any sm_86 cubin to run: one is assembled from a minimal PTX with ptxas, because CUDA 12.8's nvcc
# rejects the system gcc (> 14). CUDA 12.8 is the last release whose nvdisasm still yields the text.
#
# Usage: tools/shaders/spec.sh   (needs scratchpad/nvidia/cuda from tools/shaders/README.md; ~1 min)
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cuda=$root/scratchpad/nvidia/cuda
src=$root/scratchpad/ext/nvsass-disassembler
out=$root/build/sm86
pin=28f9064

[[ -x $cuda/bin/nvdisasm && -x $cuda/bin/ptxas ]] || { echo "missing $cuda (see tools/shaders/README.md)" >&2; exit 1; }
if [[ ! -d $src ]]; then
    git clone -q --recurse-submodules https://github.com/reoLantern/nvsass-disassembler "$src"
fi
git -C "$src" checkout -q "$pin"
git -C "$src" submodule update -q --init

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$src"/DocumentSASS/{Makefile,intercept.c,funnel.py} "$work"/
cat > "$work/k.ptx" <<'EOF'
.version 8.7
.target sm_86
.address_size 64
.visible .entry k(.param .u64 p)
{
  .reg .b64 %rd<2>;
  .reg .b32 %r<2>;
  ld.param.u64 %rd1, [p];
  mov.u32 %r1, %tid.x;
  st.global.u32 [%rd1], %r1;
  ret;
}
EOF
"$cuda/bin/ptxas" -arch=sm_86 "$work/k.ptx" -o "$work/sm_86.cubin"
make -s -C "$work" NVDISASM="$cuda/bin/nvdisasm" sm_86_instructions.txt 2>/dev/null
mkdir -p "$out"
mv "$work"/sm_86_instructions.txt "$work"/sm_86_latencies.txt "$out"/
# Exit 1 means "something was reported", not "no spec": the one known report (a missing separator in
# f2f_f32_upconvert__RCR_RCR) is repaired by the parser itself, so anything beyond it fails the build.
rc=0
python3 "$src/parse_sidl.py" "$out/sm_86_instructions.txt" -o "$out" > "$out/parse.log" || rc=$?
if (( rc > 1 )) || ! grep -q '^unparsed / suspect items: [01]$' "$out/parse.log"; then
    cat "$out/parse.log" >&2
    exit 1
fi
echo "wrote $out/instructions.json"

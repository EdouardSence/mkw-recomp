# runtime/gfx/vk/diag

Back end diagnostics and measurements: frame dump, GPU time per pass and the submit timeline.

| File | What it is |
|---|---|
| `vk_timeline.c` | `MKW_TRACE_TL=1` (measurement): each frame's submits on a single clock (game, replay, calibrated GPU, fence, waits), one line every 600 frames |
| `vk_dump.c`, `vk_dump_internal.h` | `MKW_DUMP=<frame>`: dumps a frame to PNG (RenderDoc does not capture this back end): passes, draws and dispatches; the header is what the parts share |
| `vk_dump_img.c` | Part of the dump: formats, copy of each image to a buffer and its PNGs |
| `vk_dump_buf.c` | Part of the dump: `MKW_DUMP_BUF`, a buffer range after each pass |
| `vk_passtime.c` | MKW's own: `MKW_PASS_TIMING`, GPU time by pass size (`=draw`: per program pair); measurement only |
| `vk_cs_hash.c` | `MKW_CS_HASH=<caller>`: hash of each input (storage, indirect arguments, uniforms) of the dispatches from that point of the game, before and after running; cactus flicker diagnostic |
| `vk_ubo_watch.c` | `MKW_UBO_WATCH=<address>`: the uniform block at that address as the replay binds it (the submit's copy) and whether game memory already held something else, once per frame; cactus flicker diagnostic |

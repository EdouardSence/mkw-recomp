# runtime/gfx/nvn/host

The host graphics layer: its interface and the null back end, present and window, the programs' prepared SPIR-V and
the texts the host shows.

| File | What it is |
|---|---|
| `gfx.h`, `gfx_null.c` | Graphics layer interface, null back end (no GPU) and `gfx_backend`, which picks the back end and wraps it with the capture of `../cap/` (`MKW_CAPTURE`) |
| `gfx_present.c`, `gfx_wsys.h`, `gfx_window_sdl.c` | Swapchain, present and vsync; SDL3 window |
| `gfx_shader.c` | Loads MKW's prepared SPIR-V (`prepared_get`, key `nvn2_program_key`), variants with integer inputs and reflection (`.res.json` and the SPIR-V itself); loads the pipeline prewarm shaders in parallel (`gfx_shader_prewarm`) |
| `ui_text.c`, `ui_text.h` | Texts the host shows the player (software keyboard, menu and start screen; menu in English, Russian and Spanish) |
| `ui_text_en.c`, `ui_text_ru.c`, `ui_text_es.c`, `ui_text_cat.h` | The menu texts of each language |

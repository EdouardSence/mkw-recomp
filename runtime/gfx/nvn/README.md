# runtime/gfx/nvn

NVN front end: implements the `nvn*` functions the game calls on our own objects stored inside the game's objects,
and records what they ask for as operation logs that the back end (`../vk/`) replays. It comes from the MacNativeGames
front end (origin d2r-mobile, MIT) that totk-pc-recomp uses; MKW's own part is NVN2. Every subfolder with headers is on
the include path (`tools/build/build.py`): `#include` goes by name.

| Folder | Contents |
|---|---|
| [api/](api/README.md) | The `nvn*` functions: device, queues, pools, objects, command buffers recorded as operations, ordering of the two queues |
| [procs/](procs/README.md) | NVN1 and NVN2 function tables (`nvnDeviceGetProcAddress`) and NVN2's own functions |
| [tex/](tex/README.md) | Block-linear textures: sizes, deswizzle and CPU ASTC |
| [host/](host/README.md) | Host graphics layer: interface, null back end, present and window, prepared SPIR-V, host texts |

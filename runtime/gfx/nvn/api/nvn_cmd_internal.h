// Internal interface of the command-buffer front end (nvn_api_cmd.c and nvn_api_state.c): the record helpers the
// render-state entry points use.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"

static inline cmd_t *cmd_of(const Cpu *c) { return arg_p(c, 0); }

void *cmd_rec(cmd_t *o, uint32_t op, size_t payload);   // a zeroed payload of one record
#define REC(o, OP, T) ((T *)cmd_rec((o), (OP), sizeof(T)))
// Array-carrying binds take the prefix fields plus n elements; the replay reads only the first n.
#define RECN(o, OP, T, ARR, N) ((T *)cmd_rec((o), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
void cmd_hw_fix(cmd_t *o, size_t bytes);
void cmd_copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v);
const tex_t *cmd_tex_pin(const NVNtexture *nt);

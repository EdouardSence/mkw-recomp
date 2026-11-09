// What memory.c shares with memory_early.c (MKW_EARLY_READ): the trap map and its lock. Not for other files.
#ifndef MEMORY_INT_H
#define MEMORY_INT_H
#include <stdint.h>

#include "platform/platform.h"

extern PlatMutex frz_lock;
extern uint8_t *trapped;                  // per guest page: 1 a read trap (memory.c), 2 an early-read watch, 3 a base
                                          // trap (memory_btrap.c)
extern int64_t mem_early_pages;           // frz_lock: pages watched now
int watched(uint64_t addr, uint64_t size);
int mem_trap_init(void);                  // the trap map; 0 when traps are unavailable (Windows)
uint64_t mem_early_release_locked(uint64_t a, int access);   // the fence to wait for once unlocked, 0 none
void mem_thaw_regions_locked(uint64_t a, uint64_t n);   // frz_lock held: no region over [a, a+n) stays frozen
void mem_base_fill_locked(uint64_t p);   // frz_lock held: a base-trapped page copied to its base, then writable
void mem_early_filled_locked(uint64_t a, int access);   // a trapped page was filled from the live shadow
// memory_early.c's state its diagnosis reads (memory_early_diag.c)
extern uint64_t *mem_lastf, *mem_prevf;                         // per page: the newest submit listed writing it, the one before
extern uint64_t (*mem_fence_done)(void), (*mem_fence_queued)(void);
extern uint64_t mem_early_frame;                                // the frame mem_early_log saw last
void mem_early_ev_note(int kind, uint64_t addr, uint64_t w, uint64_t done, uint64_t pc);   // MKW_EARLY_READ_DRY
void mem_early_ev_log(void);                                    // every frame
int mem_early_ev_counts(char *b, int n);                        // the 600-frame line's counts, then reset
void mem_early_wait(uint64_t fence);   // frz_lock held: a's page gets its protection back; access:
                                                         // a fault at a (the byte touched)
#endif

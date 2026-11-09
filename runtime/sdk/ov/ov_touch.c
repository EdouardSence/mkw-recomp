// nn::hid touch screen for MKW. Touch is an optional console feature (README "Decisiones"), off: almost no PC has a
// touch screen. Initialization does nothing and every read is one sample with no touches, so the game behaves as if
// nobody touched the screen. totk-pc-recomp has no touch overrides (TotK never reads the touch screen).
#include <string.h>

#include "host.h"

// TouchScreenState<16>: sampling number (8), touch count (4), reserved (4), 16 TouchState of 0x28 bytes.
#define TOUCH_STATE_16_SIZE (0x10 + 16 * 0x28)

static uint64_t samples;

// void nn::hid::InitializeTouchScreen()
static void ov_initialize_touch(Cpu *c) { (void)c; }

// int nn::hid::GetTouchScreenStates<16>(TouchScreenState<16> *out, int count): the newest sample first.
static void ov_touch_states(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    int count = (int)arg_x(c, 1);
    if (!out || count < 1) {
        ret_x(c, 0);
        return;
    }
    memset(out, 0, TOUCH_STATE_16_SIZE);
    uint64_t n = __atomic_add_fetch(&samples, 1, __ATOMIC_RELAXED);
    memcpy(out, &n, sizeof n);
    ret_x(c, 1);
}

HOST_OVERRIDE("_ZN2nn3hid21InitializeTouchScreenEv", ov_initialize_touch);
HOST_OVERRIDE("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", ov_touch_states);

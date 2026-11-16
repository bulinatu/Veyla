#pragma once
// Minimal stand-in for gf-complete's galois.h. chiaki_init() only calls
// galois_init_default_field() to set up FEC; registration never uses FEC.
#include <errno.h>

static inline int galois_init_default_field(int w) {
    (void)w;
    return 0;
}

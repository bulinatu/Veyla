// AIA (certificate issuer fetching) is a curl-based feature we do not build.
// common.c references chiaki_aia_init() from chiaki_init(), so provide the two
// lifecycle symbols and nothing else.
#include "aia.h"

ChiakiErrorCode chiaki_aia_init(void) {
    return CHIAKI_ERR_SUCCESS;
}

void chiaki_aia_fini(void) {
}

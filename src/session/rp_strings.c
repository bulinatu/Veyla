// The RP version/reason string helpers live in chiaki's session.c, which drags
// in the whole streaming stack. Registration only needs these two pure
// functions, so they are reproduced here (same project, same licence).
#include <chiaki/session.h>

const char *chiaki_rp_application_reason_string(uint32_t reason) {
    switch (reason) {
        case CHIAKI_RP_APPLICATION_REASON_REGIST_FAILED:
            return "Regist failed, probably invalid PIN";
        case CHIAKI_RP_APPLICATION_REASON_INVALID_PSN_ID:
            return "Invalid PSN ID";
        case CHIAKI_RP_APPLICATION_REASON_IN_USE:
            return "Remote is already in use";
        case CHIAKI_RP_APPLICATION_REASON_CRASH:
            return "Remote Play on Console crashed";
        case CHIAKI_RP_APPLICATION_REASON_RP_VERSION:
            return "RP-Version mismatch";
        default:
            return "unknown";
    }
}

const char *chiaki_rp_version_string(ChiakiTarget version) {
    switch (version) {
        case CHIAKI_TARGET_PS4_8:
            return "8.0";
        case CHIAKI_TARGET_PS4_9:
            return "9.0";
        case CHIAKI_TARGET_PS4_10:
            return "10.0";
        case CHIAKI_TARGET_PS5_1:
            return "1.0";
        default:
            return NULL;
    }
}

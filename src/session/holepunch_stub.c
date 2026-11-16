// Internet (PSN holepunching) support is out of scope for LAN streaming and
// needs libevent, which is not part of this build. These stubs satisfy the
// linker; the local path never calls into them because holepunch_session is
// NULL in ChiakiConnectInfo.
#include <chiaki/remote/holepunch.h>

#include <string.h>

ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session) {
    (void)session;
    ChiakiHolepunchRegistInfo info;
    memset(&info, 0, sizeof(info));
    return info;
}

void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip) {
    (void)session;
    if (ps_ip) {
        ps_ip[0] = '\0';
    }
}

uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session) {
    (void)session;
    return 0;
}

chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session,
                                           ChiakiHolepunchPortType type) {
    (void)session;
    (void)type;
    return NULL;
}

ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session) {
    (void)session;
    return CHIAKI_ERR_UNKNOWN;
}

ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session,
                                                    ChiakiHolepunchPortType port_type) {
    (void)session;
    (void)port_type;
    return CHIAKI_ERR_UNKNOWN;
}

void chiaki_holepunch_session_fini(ChiakiHolepunchSession session) {
    (void)session;
}

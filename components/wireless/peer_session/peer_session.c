#include "peer_session.h"

#include <errno.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "node.h"

#define HELLO_SIZE 24
#define HELLO_VERSION 1
#define IO_TIMEOUT_MS 5000
#define RENEW_MS 5000

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

bool peer_session_init(peer_session_t *session) {
    memset(session, 0, sizeof(*session));
    session->sock = -1;
    return mutex_create(&session->write_lock);
}

void peer_session_start(peer_session_t *session) {
    WITH_LOCK(&session->write_lock, { session->stopping = false; });
}

void peer_session_shutdown(peer_session_t *session) {
    WITH_LOCK(&session->write_lock, {
        session->stopping = true;
        if (session->sock >= 0) shutdown(session->sock, SHUT_RDWR);
    });
}

static bool usable(peer_session_t *session) {
    bool result;
    WITH_LOCK(&session->write_lock, { result = !session->stopping && session->sock >= 0; });
    return result;
}

static bool write_all(int sock, const uint8_t *data, unsigned len) {
    uint64_t deadline = now_ms() + IO_TIMEOUT_MS;
    while (len) {
        if (now_ms() >= deadline) return false;
        int sent = send(sock, data, len, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        len -= sent;
    }
    return true;
}

/* Idle connections remain open. A partial frame has a bounded completion time. */
static bool read_exact(peer_session_t *session, uint8_t *data, unsigned len, bool idle) {
    uint64_t deadline = now_ms() + IO_TIMEOUT_MS;
    while (len && usable(session)) {
        uint64_t now = now_ms();
        if (!idle && now >= deadline) return false;
        if (session->admitted) {
            if (!nm_owns(&session->link)) return false;
            if (now - session->renewed_ms >= RENEW_MS) {
                if (!nm_renew(&session->link)) return false;
                session->renewed_ms = now_ms();
            }
        }
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(session->sock, &readers);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        int ready = lwip_select(session->sock + 1, &readers, NULL, NULL, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return false;
        if (!ready) continue;
        int received = recv(session->sock, data, len, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        if (idle) { idle = false; deadline = now_ms() + IO_TIMEOUT_MS; }
        data += received;
        len -= received;
    }
    return len == 0;
}

static bool vote(peer_session_t *session, bool accepted) {
    uint8_t local = accepted ? 1 : 0, remote = 0;
    return usable(session) && write_all(session->sock, &local, 1) &&
           read_exact(session, &remote, 1, false) && local == 1 && remote == 1;
}

bool peer_session_open(peer_session_t *session, int sock, bool station, const char *expected_ssid) {
    bool started = false;
    WITH_LOCK(&session->write_lock, {
        if (!session->stopping && session->sock < 0) {
            session->sock = sock;
            session->admitted = false;
            memset(&session->link, 0, sizeof(session->link));
            started = true;
        }
    });
    if (!started) return false;

    struct timeval timeout = { .tv_sec = IO_TIMEOUT_MS / 1000 };
    int keepalive = 1, keepidle = 5, keepinterval = 5, keepcount = 3;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) ||
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) ||
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepinterval, sizeof(keepinterval)) ||
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcount, sizeof(keepcount)))
        return false;

    uint8_t hello[HELLO_SIZE] = { 'I', '4', 'A', 'N', HELLO_VERSION, station ? 1 : 0,
                                node_get_device_orientation(), 0 };
    const char *uuid = node_get_uuid();
    if (!nm_uuid_valid(uuid) || hello[6] >= NM_RADIOS) return false;
    memcpy(hello + 8, uuid, NM_UUID_LEN);
    uint32_t token = station ? esp_random() : 0;
    if (station && !token) token = 1;
    hello[20] = token >> 24; hello[21] = token >> 16; hello[22] = token >> 8; hello[23] = token;
    uint8_t remote[HELLO_SIZE];
    if (!write_all(sock, hello, sizeof(hello)) || !read_exact(session, remote, sizeof(remote), false) ||
        memcmp(remote, "I4AN", 4) || remote[4] != HELLO_VERSION ||
        remote[5] != (station ? 0 : 1) || remote[6] >= NM_RADIOS || remote[7] != 0)
        return false;
    char peer_uuid[NM_UUID_LEN + 1] = {0};
    memcpy(peer_uuid, remote + 8, NM_UUID_LEN);
    if (!nm_uuid_valid(peer_uuid) || !memcmp(peer_uuid, uuid, NM_UUID_LEN + 1)) return false;
    if (station) {
        char scanned_uuid[NM_UUID_LEN + 1];
        uint8_t scanned_orientation;
        if (!nm_parse_ssid(expected_ssid, scanned_uuid, &scanned_orientation) ||
            memcmp(scanned_uuid, peer_uuid, NM_UUID_LEN + 1) || scanned_orientation != remote[6] ||
            remote[20] || remote[21] || remote[22] || remote[23])
            return false;
    } else {
        token = (uint32_t)remote[20] << 24 | (uint32_t)remote[21] << 16 |
                (uint32_t)remote[22] << 8 | remote[23];
        if (!token) return false;
    }
    nm_link_id_t *link = &session->link;
    memcpy(link->initiator_uuid, station ? uuid : peer_uuid, NM_UUID_LEN + 1);
    link->session = token;
    link->station_orientation = station ? hello[6] : remote[6];
    link->ap_orientation = station ? remote[6] : hello[6];

    if (!vote(session, nm_reserve(peer_uuid, link)) ||
        !vote(session, usable(session) && nm_commit(link)) ||
        !vote(session, usable(session) && nm_activate(link)) || !nm_owns(link))
        return false;
    WITH_LOCK(&session->write_lock, {
        session->admitted = !session->stopping;
        started = session->admitted;
    });
    session->renewed_ms = now_ms();
    return started;
}

void peer_session_close(peer_session_t *session) {
    WITH_LOCK(&session->write_lock, {
        session->admitted = false;
        if (session->sock >= 0) shutdown(session->sock, SHUT_RDWR);
        session->sock = -1;
    });
    if (session->link.session) nm_release(&session->link);
    memset(&session->link, 0, sizeof(session->link));
}

int peer_session_receive(peer_session_t *session, uint8_t *buffer, uint16_t capacity) {
    uint8_t header[2];
    if (!session->admitted || !read_exact(session, header, sizeof(header), true)) return -1;
    uint16_t len = (uint16_t)header[0] << 8 | header[1];
    if (!len || len > PEER_SESSION_MAX_MESSAGE || len > capacity ||
        !read_exact(session, buffer, len, false))
        return -1;
    return len;
}

bool peer_session_send(peer_session_t *session, const uint8_t *data, uint16_t len) {
    if (!data || !len || len > PEER_SESSION_MAX_MESSAGE) return false;
    bool sent = false;
    uint8_t header[2] = { len >> 8, len };
    WITH_LOCK(&session->write_lock, {
        if (session->admitted && !session->stopping && nm_owns(&session->link))
            sent = write_all(session->sock, header, sizeof(header)) && write_all(session->sock, data, len);
        if (!sent && session->sock >= 0) shutdown(session->sock, SHUT_RDWR);
    });
    return sent;
}

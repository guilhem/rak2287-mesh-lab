#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "sx1250_dual.h"
#include "sx1250_tx.h"

enum { N_RADIOS = 2, TX_WAIT_MS = 2000 };
enum { TX_OK, TX_BUSY, TX_DISABLED, TX_ERROR };

static struct radio {
    const char *name;
    struct sx1250_rx_config profile;
    int listener, peer;
    struct sockaddr_un address;
    uint8_t pending[255];
    size_t size;
    int64_t deadline;
} radios[N_RADIOS] = {
    {.name = "meshtastic", .profile = {869300000, 250000, 16, 9, 5, 0x2b, true, false, 0},
     .listener = -1, .peer = -1},
    {.name = "meshcore", .profile = {869618000, 62500, 16, 8, 8, 0x12, true, false, 0},
     .listener = -1, .peer = -1},
};

static volatile sig_atomic_t stopping;
static void stop(int signum) { (void)signum; stopping = 1; }

static int64_t clock_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void disconnect(struct radio *radio) {
    if (radio->peer >= 0) close(radio->peer);
    radio->peer = -1;
    radio->size = 0;
}

static void respond(struct radio *radio, uint8_t status) {
    uint8_t response[] = {'A', status};
    if (send(radio->peer, response, sizeof response, MSG_NOSIGNAL) != sizeof response)
        disconnect(radio);
}

static int listen_at(struct radio *radio, const char *dir) {
    radio->address.sun_family = AF_UNIX;
    char *path = radio->address.sun_path;
    const size_t capacity = sizeof radio->address.sun_path;
    const int length = snprintf(path, capacity, "%s/%s.sock", dir, radio->name);
    if (length < 0 || (size_t)length >= capacity) { errno = ENAMETOOLONG; return -1; }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&radio->address, sizeof radio->address)) {
        close(fd);
        return -1;
    }
    radio->listener = fd;
    return listen(fd, 1);
}

static void accept_peer(struct radio *radio) {
    int peer = accept4(radio->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (peer < 0) return;
    if (radio->peer >= 0) close(peer);
    else radio->peer = peer;
}

static void receive_request(struct radio *radio, bool allow_tx, int64_t now) {
    uint8_t frame[256];
    ssize_t size = recv(radio->peer, frame, sizeof frame, MSG_TRUNC);
    if (size < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (size <= 1 || size > (ssize_t)sizeof frame || frame[0] != 'T' || radio->size) {
        disconnect(radio);
        return;
    }
    if (!allow_tx) { respond(radio, TX_DISABLED); return; }
    radio->size = (size_t)size - 1;
    memcpy(radio->pending, frame + 1, radio->size);
    radio->deadline = now + TX_WAIT_MS;
}

static int forward_rx(unsigned rf) {
    struct sx1250_rx_packet packet;
    int rc = sx1250_dual_next(rf, &packet);
    struct radio *radio = &radios[rf];
    if (rc <= 0 || radio->peer < 0 || packet.coding_rate != radio->profile.coding_rate ||
        !packet.crc_present || packet.implicit_header) return rc;
    const uint16_t rssi = (int16_t)(packet.rssi_dbm * 4);
    const uint16_t snr = (int16_t)(packet.snr_db * 4);
    uint8_t frame[5 + sizeof packet.payload] = {'R', rssi >> 8, rssi, snr >> 8, snr};
    memcpy(frame + 5, packet.payload, packet.size);
    if (send(radio->peer, frame, 5 + packet.size, MSG_NOSIGNAL) < 0 &&
        errno != EAGAIN && errno != ENOBUFS && errno != EINTR)
        disconnect(radio);
    return rc;
}

static int run(bool allow_tx) {
    unsigned turn = 0;
    int64_t next_send = 0;
    while (!stopping) {
        struct pollfd pollset[2 * N_RADIOS];
        for (unsigned i = 0; i < N_RADIOS; ++i) {
            pollset[2 * i] = (struct pollfd){radios[i].listener, POLLIN, 0};
            pollset[2 * i + 1] = (struct pollfd){radios[i].peer, POLLIN, 0};
        }
        if (poll(pollset, 2 * N_RADIOS, 2) < 0 && errno != EINTR) return -1;
        int64_t now = clock_ms();
        if (now < 0) return -1;
        for (unsigned i = 0; i < N_RADIOS; ++i) {
            const short events = pollset[2 * i + 1].revents;
            if (events & (POLLHUP | POLLERR | POLLNVAL)) disconnect(&radios[i]);
            else if (events & POLLIN) receive_request(&radios[i], allow_tx, now);
            if (pollset[2 * i].revents & POLLIN) accept_peer(&radios[i]);
            if (forward_rx(i) < 0) return -1;
            if (radios[i].size && now >= radios[i].deadline) {
                radios[i].size = 0;
                respond(&radios[i], TX_BUSY);
            }
        }
        if (stopping || now < next_send) continue;
        for (unsigned i = 0; i < N_RADIOS; ++i) {
            unsigned index = (turn + i) % N_RADIOS;
            struct radio *radio = &radios[index];
            if (!radio->size) continue;
            const uint32_t airtime = sx1250_lora_airtime_ms(&radio->profile, radio->size);
            if (!airtime) {
                radio->size = 0;
                respond(radio, TX_ERROR);
                continue;
            }
            struct sx1250_handoff_report report = {0};
            int rc = sx1250_dual_send(&radio->profile, radio->pending, radio->size,
                                     2 * airtime + 500, &report);
            if (rc < 0 && errno == EAGAIN && !report.tx_started) break;
            int error = rc < 0 ? errno : 0;
            radio->size = 0;
            respond(radio, rc == 0 ? TX_OK : TX_ERROR);
            turn = (index + 1) % N_RADIOS;
            if (report.tx_started) {
                int64_t ended = clock_ms();
                if (ended < 0) return -1;
                next_send = ended + 9 * (int64_t)airtime;
            }
            fprintf(stderr, "TX profile=%s airtime_ms=%u tx_done=%d rf0=%d rf1=%d error=%d\n",
                    radio->name, airtime, report.tx_done, report.rf0_rearmed,
                    report.rf1_rearmed, error);
            if (rc < 0 && (!report.rf0_rearmed || !report.rf1_rearmed)) {
                errno = error;
                return -1;
            }
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) goto usage;
    if (argc == 4 && strcmp(argv[3], "--enable-transmit")) goto usage;
    const bool allow_tx = argc == 4;
    struct stat st;
    if (argv[1][0] != '/' || lstat(argv[1], &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 077)) {
        fputs("Socket directory must be an absolute, private directory owned by the broker.\n", stderr);
        return 1;
    }
    umask(0077);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    int rc = 1;
    for (unsigned i = 0; i < N_RADIOS; ++i) {
        if (listen_at(&radios[i], argv[1])) { perror("listen_at"); goto cleanup; }
    }
    struct sx1250_rx_config profiles[] = {radios[0].profile, radios[1].profile};
    if (sx1250_dual_open(argv[2], profiles)) { perror("sx1250_dual_open"); goto cleanup; }
    printf("RADIO_BROKER_READY meshtastic_hz=%u meshcore_hz=%u tx_enabled=%d\n",
           profiles[0].frequency_hz, profiles[1].frequency_hz, allow_tx);
    fflush(stdout);
    rc = run(allow_tx) < 0;
    if (rc) perror("radio broker");
    if (sx1250_dual_close()) { perror("sx1250_dual_close"); rc = 1; }
cleanup:
    for (unsigned i = 0; i < N_RADIOS; ++i) {
        disconnect(&radios[i]);
        if (radios[i].listener < 0) continue;
        close(radios[i].listener);
        unlink(radios[i].address.sun_path);
    }
    return rc;
usage:
    fputs("Usage: radio-broker SOCKET_DIRECTORY SPI_DEVICE [--enable-transmit]\n", stderr);
    return 2;
}

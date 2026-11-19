#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sx1250_dual.h"
#include "sx1250_tx.h"

static int64_t start;
static bool delivered[2], receiving;

static int64_t now_ms(void) {
    struct timespec time;
    assert(!clock_gettime(CLOCK_MONOTONIC, &time));
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

int sx1250_dual_open(const char *path, const struct sx1250_rx_config profiles[2]) {
    (void)path;
    assert(profiles[0].bandwidth_hz == 250000 && profiles[1].bandwidth_hz == 62500);
    start = now_ms();
    receiving = true;
    return 0;
}

int sx1250_dual_close(void) { return 0; }

int sx1250_dual_next(unsigned rf, struct sx1250_rx_packet *packet) {
    assert(receiving);
    if (!getenv("FAKE_RX") || delivered[rf] || now_ms() - start < 250) return 0;
    delivered[rf] = true;
    const uint8_t bytes[] = {0x15, 0x40, 0xaa, 1, 2, 3, 4, 5, 6, 7};
    *packet = (struct sx1250_rx_packet){
        .size=sizeof bytes, .coding_rate=rf == 0 ? 5 : 8,
        .crc_present=true, .rssi_dbm=-95.5, .snr_db=4.25
    };
    memcpy(packet->payload, bytes, sizeof bytes);
    return 1;
}

uint32_t sx1250_lora_airtime_ms(const struct sx1250_rx_config *profile, uint8_t size) {
    (void)profile;
    return size ? 10 : 0;
}

int sx1250_dual_send(const struct sx1250_rx_config *profile, const uint8_t *payload,
                    size_t length, unsigned timeout, struct sx1250_handoff_report *report) {
    assert(receiving && timeout >= 500);
    if (getenv("FAKE_ALWAYS_BUSY") || (getenv("FAKE_RX") && now_ms() - start < 250)) {
        errno = EAGAIN;
        return -1;
    }
    receiving = false;
    printf("SET_TX hz=%u bw=%u payload=", profile->frequency_hz, profile->bandwidth_hz);
    for (size_t i = 0; i < length; ++i) printf("%02x", payload[i]);
    puts("");
    fflush(stdout);
    nanosleep(&(struct timespec){.tv_nsec=20000000}, NULL);
    *report = (struct sx1250_handoff_report){
        .tx_started=true, .tx_done=!getenv("FAKE_AMBIGUOUS"),
        .rf0_rearmed=true, .rf1_rearmed=true
    };
    receiving = true;
    if (!report->tx_done) { errno = EIO; return -1; }
    return 0;
}

#pragma once
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "sx1250_rx.h"

class BrokerRadio {
    int fd = -1;
    std::deque<sx1250_rx_packet> pending;
    static int unpack(const uint8_t *frame, ssize_t size, sx1250_rx_packet& packet) {
        if (size < 6 || size > 260 || frame[0] != 'R') { errno = EPROTO; return -1; }
        auto metric = [](const uint8_t *p) {
            unsigned bits = (p[0] << 8) | p[1];
            return (static_cast<int>(bits) - ((bits & 0x8000) ? 65536 : 0)) / 4.0f;
        };
        packet = {};
        packet.size = size - 5;
        packet.coding_rate = 8;
        packet.crc_present = true;
        packet.rssi_dbm = metric(frame + 1);
        packet.snr_db = metric(frame + 3);
        std::memcpy(packet.payload, frame + 5, packet.size);
        return 1;
    }

    int read(uint8_t *frame) {
        ssize_t size = recv(fd, frame, 260, MSG_DONTWAIT | MSG_TRUNC);
        if (size == 0) { errno = ECONNRESET; return -1; }
        if (size > 260) { errno = EPROTO; return -1; }
        return size;
    }
public:
    bool connected() const { return fd >= 0; }
    bool hasPending() const { return !pending.empty(); }

    int open(const char *path) {
        const char *base = std::strrchr(path, '/');
        if (!base || path[0] != '/' || std::strcmp(base + 1, "meshcore.sock")) {
            errno = EINVAL;
            return -1;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto len = std::strlen(path);
        if (len >= sizeof address.sun_path) { errno = ENAMETOOLONG; return -1; }
        std::memcpy(address.sun_path, path, len + 1);
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0) return 0;
        const int error = errno;
        close();
        errno = error;
        return -1;
    }

    int close() {
        int result = fd >= 0 ? ::close(fd) : 0;
        fd = -1;
        pending.clear();
        return result;
    }

    int next(sx1250_rx_packet& packet) {
        if (!pending.empty()) {
            packet = pending.front();
            pending.pop_front();
            return 1;
        }
        uint8_t frame[260];
        int size = read(frame);
        if (size < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
        return unpack(frame, size, packet);
    }

    int sendPacket(const uint8_t *data, size_t length, unsigned timeout) {
        if (!data || !length || length > 255) { errno = EINVAL; return -1; }
        uint8_t frame[260] = {'T'};
        std::memcpy(frame + 1, data, length);
        if (send(fd, frame, length + 1, MSG_NOSIGNAL | MSG_DONTWAIT) !=
            static_cast<ssize_t>(length + 1)) {
            if (errno == EAGAIN || errno == ENOBUFS) errno = EBUSY;
            return -1;
        }
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(timeout);
        while (steady_clock::now() < deadline) {
            pollfd waiter{fd, POLLIN, 0};
            const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
            int ready = poll(&waiter, 1, remaining > 0 ? static_cast<int>(remaining) : 0);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) return -1;
            if (!ready) break;
            int size = read(frame);
            if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (size < 0) return -1;
            if (size == 2 && frame[0] == 'A') {
                if (frame[1] == 0) return 0;
                errno = frame[1] == 1 ? EAGAIN : frame[1] == 2 ? EPERM : EIO;
                return -1;
            }
            sx1250_rx_packet packet;
            if (unpack(frame, size, packet) < 0) return -1;
            if (pending.size() < 64) pending.push_back(packet);
        }
        errno = ETIMEDOUT;
        return -1;
    }
};

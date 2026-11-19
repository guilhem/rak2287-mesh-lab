// SPDX-License-Identifier: GPL-3.0-only
#if defined(ARCH_PORTDUINO) && defined(__linux__)

#include "MeshPacketQueue.h"
#include "NodeDB.h"
#include "RadioInterface.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "concurrency/OSThread.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

enum class TxResult : uint8_t { Sent, Busy, Disabled, Uncertain };
constexpr size_t rxPrefix = 5;
constexpr unsigned maxBusyRetries = 10;

int quarterDb(const uint8_t *bytes)
{
    const int word = 256 * bytes[0] + bytes[1];
    return word < 32768 ? word : word - 65536;
}

class UnixRadio final : public RadioInterface, private concurrency::OSThread
{
    MeshPacketQueue queue{MAX_TX_QUEUE};
    sockaddr_un address{};
    int fd = -1;
    bool configured = false, awaitingAck = false;
    unsigned busyRetries = 0;
    Clock::time_point reconnectAt{}, txAt{}, ackDeadline{};

    void discardQueued()
    {
        while (auto *p = queue.dequeue())
            packetPool.release(p);
    }

    void releaseTx()
    {
        packetPool.release(sendingPacket);
        sendingPacket = nullptr;
        awaitingAck = false;
        busyRetries = 0;
        scheduleTx();
    }

    void finishTx(TxResult result)
    {
        if (result == TxResult::Sent)
            airTime->logAirtime(TX_LOG, RadioInterface::getPacketTime(sendingPacket));
        else if (result != TxResult::Busy || busyRetries == maxBusyRetries)
            LOG_WARN("RAK2287 TX failed, broker status=%u", static_cast<unsigned>(result));
        else {
            // BUSY means the broker never ran SetTx.
            awaitingAck = false;
            ++busyRetries;
            txAt = Clock::now() + 500ms;
            return;
        }
        releaseTx();
    }

    void disconnect()
    {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        if (sendingPacket) {
            if (awaitingAck)
                LOG_WARN("RAK2287 TX outcome unknown after disconnect");
            releaseTx();
        }
        discardQueued();
        reconnectAt = Clock::now() + 1s;
    }

    void scheduleTx()
    {
        if (sendingPacket)
            return;
        if (auto *p = queue.getFront()) {
            const auto delay = p->rx_rssi ? getTxDelayMsecWeighted(p) : getTxDelayMsec();
            txAt = Clock::now() + std::chrono::milliseconds(delay);
        }
    }

    void connectBroker()
    {
        if (Clock::now() < reconnectAt)
            return;
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0 || connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
            disconnect();
            return;
        }
        LOG_INFO("RAK2287 packet socket connected");
    }

    void applyBrokerProfile()
    {
        configured = config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET &&
                     std::abs(getFreq() - 869.300f) < 0.0005f && bw == 250 && sf == 9 && cr == 5 &&
                     preambleLength == 16;
        if (!configured) {
            disconnect();
            LOG_WARN("RAK2287 socket requires 869.300 MHz Medium Fast (SF9, 250 kHz, CR4/5)");
        }
    }

    void receiveFrame(const uint8_t *bytes, size_t length, int rssi, int snr)
    {
        if (!router || length < sizeof(PacketHeader) || length > MAX_LORA_PAYLOAD_LEN)
            return;
        PacketHeader header;
        std::memcpy(&header, bytes, sizeof(header));
        if (!header.from)
            return;
        auto *packet = packetPool.allocZeroed();
        if (!packet)
            return;

        packet->from = header.from;
        packet->to = header.to;
        packet->id = header.id;
        packet->channel = header.channel;
        packet->hop_limit = header.flags & PACKET_FLAGS_HOP_LIMIT_MASK;
        packet->hop_start = (header.flags & PACKET_FLAGS_HOP_START_MASK) >> PACKET_FLAGS_HOP_START_SHIFT;
        packet->want_ack = header.flags & PACKET_FLAGS_WANT_ACK_MASK;
        packet->via_mqtt = header.flags & PACKET_FLAGS_VIA_MQTT_MASK;
        packet->next_hop = packet->hop_start ? header.next_hop : NO_NEXT_HOP_PREFERENCE;
        packet->relay_node = packet->hop_start ? header.relay_node : NO_RELAY_NODE;
        packet->rx_rssi = std::lround(rssi / 4.0);
        packet->has_rx_rssi = true;
        packet->rx_snr = snr / 4.0f;
        packet->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        packet->encrypted.size = length - sizeof(header);
        std::memcpy(packet->encrypted.bytes, bytes + sizeof(header), packet->encrypted.size);
        airTime->logAirtime(RX_LOG, getPacketTime(length, true));
        deliverToReceiver(packet);
    }

    void readBroker()
    {
        std::array<uint8_t, rxPrefix + MAX_LORA_PAYLOAD_LEN> data;
        for (unsigned read = 0; read < 16; ++read) {
            const ssize_t count = recv(fd, data.data(), data.size(), MSG_TRUNC);
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            if (count > static_cast<ssize_t>(rxPrefix) && static_cast<size_t>(count) <= data.size() && data[0] == 'R') {
                receiveFrame(data.data() + rxPrefix, count - rxPrefix, quarterDb(data.data() + 1),
                             quarterDb(data.data() + 3));
            } else if (count == 2 && data[0] == 'A' && data[1] <= static_cast<uint8_t>(TxResult::Uncertain) &&
                       awaitingAck) {
                finishTx(static_cast<TxResult>(data[1]));
            } else {
                disconnect();
                return;
            }
        }
    }

    void writeBroker()
    {
        auto *p = sendingPacket ? sendingPacket : queue.getFront();
        const auto now = Clock::now();
        if (!p || awaitingAck || now < txAt ||
            (p->tx_after && !Throttle::deadlinePassedAt(Time::getMillis(), p->tx_after)))
            return;

        std::array<uint8_t, 1 + MAX_LORA_PAYLOAD_LEN> data;
        data[0] = 'T';
        const auto size = encodeRadioBuffer(p);
        if (size > MAX_LORA_PAYLOAD_LEN) {
            disconnect();
            return;
        }
        std::memcpy(data.data() + 1, &radioBuffer, size);
        const ssize_t written = ::send(fd, data.data(), size + 1, MSG_NOSIGNAL);
        if (written == static_cast<ssize_t>(size + 1)) {
            if (!sendingPacket)
                sendingPacket = queue.dequeue();
            awaitingAck = true;
            // Allow another profile's TX timeout, admission wait, and RX recovery.
            ackDeadline = now + 12s;
        } else if (written >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            disconnect();
        }
    }

    int32_t runOnce() override
    {
        if (disabled || !configured)
            return 100;
        if (!config.lora.tx_enabled && awaitingAck)
            disconnect();
        if (fd < 0)
            connectBroker();
        if (fd >= 0)
            readBroker();
        if (awaitingAck && Clock::now() >= ackDeadline)
            disconnect();
        if (!config.lora.tx_enabled) {
            discardQueued();
            if (sendingPacket && !awaitingAck)
                releaseTx();
        } else if (fd >= 0)
            writeBroker();
        return 5;
    }

    bool matchesPending(NodeNum from, PacketId id)
    {
        return sendingPacket && getFrom(sendingPacket) == from && sendingPacket->id == id;
    }

    bool matchesRetry(NodeNum from, PacketId id) { return !awaitingAck && matchesPending(from, id); }

  public:
    UnixRadio() : OSThread("RAK2287", 5) {}

    ~UnixRadio() override { disconnect(); }

    bool init() override
    {
        const char *path = std::getenv("MESHTASTIC_RADIO_SOCKET");
        if (!path || path[0] != '/' || std::strlen(path) >= sizeof(address.sun_path) ||
            std::strcmp(std::strrchr(path, '/') + 1, "meshtastic.sock") != 0) {
            LOG_ERROR("MESHTASTIC_RADIO_SOCKET must be an absolute path ending in /meshtastic.sock");
            return false;
        }
        address.sun_family = AF_UNIX;
        std::strcpy(address.sun_path, path);
        if (!RadioInterface::init())
            return false;
        applyBrokerProfile();
        return true;
    }

    bool reconfigure() override
    {
        if (!RadioInterface::reconfigure())
            return false;
        applyBrokerProfile();
        return true;
    }

    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        if (disabled || !configured || !config.lora.tx_enabled) {
            packetPool.release(p);
            return ERRNO_DISABLED;
        }
        if (p->to == NODENUM_BROADCAST_NO_LORA)
            return ERRNO_SHOULD_RELEASE;
#ifdef LORA_DISABLE_SENDING
        packetPool.release(p);
        return ERRNO_DISABLED;
#else
        if (fd < 0 ||
            p->which_payload_variant != meshtastic_MeshPacket_encrypted_tag ||
            p->encrypted.size > MAX_RADIO_PAYLOAD_LEN) {
            packetPool.release(p);
            return ERRNO_DISABLED;
        }
        const bool idle = queue.empty();
        if (!queue.enqueue(p)) {
            packetPool.release(p);
            return ERRNO_UNKNOWN;
        }
        if (idle)
            scheduleTx();
        return ERRNO_OK;
#endif
    }

    meshtastic_QueueStatus getQueueStatus() override
    {
        meshtastic_QueueStatus status{};
        status.free = queue.getFree();
        status.maxlen = queue.getMaxLen();
        return status;
    }

    bool cancelSending(NodeNum from, PacketId id) override
    {
        if (matchesRetry(from, id)) {
            releaseTx();
            return true;
        }
        if (auto *p = queue.remove(from, id)) {
            packetPool.release(p);
            return true;
        }
        return false;
    }

    bool findInTxQueue(NodeNum from, PacketId id) override { return matchesPending(from, id) || queue.find(from, id); }

    void clampToLateRebroadcastWindow(NodeNum from, PacketId id) override
    {
        if (matchesRetry(from, id) && !sendingPacket->tx_after) {
            sendingPacket->tx_after = Time::timerEndsAtMillis(getTxDelayMsecWeightedWorst(sendingPacket->rx_snr));
        } else if (auto *p = queue.remove(from, id, true, false)) {
            p->tx_after = Time::timerEndsAtMillis(getTxDelayMsecWeightedWorst(p->rx_snr));
            if (!queue.enqueue(p))
                packetPool.release(p);
        }
    }

    bool removePendingTXPacket(NodeNum from, PacketId id, uint32_t hopLimit) override
    {
        if (matchesRetry(from, id) && (!hopLimit || sendingPacket->hop_limit < hopLimit)) {
            releaseTx();
            return true;
        }
        if (auto *p = queue.remove(from, id, true, true, hopLimit)) {
            packetPool.release(p);
            return true;
        }
        return false;
    }

    bool canSleep(bool) override { return queue.empty() && !sendingPacket; }

    bool sleep() override
    {
        disconnect();
        return true;
    }

    uint32_t getPacketTime(uint32_t length, bool) override
    {
        const double symbolMs = std::ldexp(1.0, sf) / bw;
        const unsigned lowRate = symbolMs >= 16;
        const double payloadSymbols = std::max(0.0, std::ceil((8.0 * length - 4 * sf + 44) / (4 * (sf - 2 * lowRate))));
        return static_cast<uint32_t>(std::ceil((preambleLength + 4.25 + 8 + payloadSymbols * cr) * symbolMs));
    }
};
} // namespace

std::unique_ptr<RadioInterface> newRak2287UnixRadio()
{
    return std::make_unique<UnixRadio>();
}

#endif

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace {

namespace NetParams {
    constexpr uint16_t BindPort = 47010;
    constexpr uint16_t DestPort = 47001;
    constexpr std::size_t RawHdrSize = 4;
    constexpr std::size_t PayloadLen = 160;
    constexpr std::size_t RawPktSize = 164;
    constexpr std::size_t MinChunks = 20;
    constexpr std::size_t TotalChunks = 32;
    constexpr std::size_t ChunkLen = 8;
    constexpr std::size_t EncHdrLen = 2;
    constexpr std::size_t EncPktSize = EncHdrLen + ChunkLen;
    constexpr uint32_t ValidSeqBits = 0x07ffu;
}

uint8_t galois_multiply(uint8_t v1, uint8_t v2) {
    uint8_t product = 0;
    for (; v2 > 0; v2 >>= 1u) {
        if (v2 & 1u) {
            product ^= v1;
        }
        const bool overflow = (v1 & 0x80u) != 0;
        v1 = static_cast<uint8_t>(v1 << 1u);
        if (overflow) {
            v1 ^= 0x1du;
        }
    }
    return product;
}

bool transmit_chunk(int sock_fd, const sockaddr_in& target, const unsigned char* buf) {
    while (true) {
        const ssize_t bytes_pushed = sendto(
            sock_fd, buf, NetParams::EncPktSize, 0,
            reinterpret_cast<const sockaddr*>(&target),
            sizeof(target)
        );
        
        if (bytes_pushed == static_cast<ssize_t>(NetParams::EncPktSize)) {
            return true;
        }
        if (bytes_pushed < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

// Struct to build and hold the Vandermonde matrix used for encoding.
struct EncodingMatrix {
    uint8_t data[NetParams::TotalChunks][NetParams::MinChunks]{};

    EncodingMatrix() {
        for (std::size_t r = 0; r < NetParams::TotalChunks; ++r) {
            const uint8_t basis = static_cast<uint8_t>(r + 1);
            data[r][0] = 1;
            for (std::size_t c = 1; c < NetParams::MinChunks; ++c) {
                data[r][c] = galois_multiply(data[r][c - 1], basis);
            }
        }
    }
};

}  // namespace

int main() {
    int listener_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listener_fd < 0) {
        std::perror("socket(listen)");
        return 1;
    }

    sockaddr_in local_bind{};
    local_bind.sin_family = AF_INET;
    local_bind.sin_port = htons(NetParams::BindPort);
    local_bind.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    if (bind(listener_fd, reinterpret_cast<sockaddr*>(&local_bind), sizeof(local_bind)) < 0) {
        std::perror("bind(local)");
        close(listener_fd);
        return 1;
    }

    int sender_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender_fd < 0) {
        std::perror("socket(send)");
        close(listener_fd);
        return 1;
    }

    sockaddr_in remote_dest{};
    remote_dest.sin_family = AF_INET;
    remote_dest.sin_port = htons(NetParams::DestPort);
    remote_dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    EncodingMatrix generator;
    
    unsigned char raw_buf[65536];
    unsigned char encoded_buf[NetParams::EncPktSize];
    
    while (true) {
        const ssize_t rx_len = recvfrom(
            listener_fd, raw_buf, sizeof(raw_buf), 0, nullptr, nullptr
        );
        
        if (rx_len < 0) {
            if (errno == EINTR) continue;
            std::perror("recvfrom(raw)");
            break;
        }
        
        if (rx_len != static_cast<ssize_t>(NetParams::RawPktSize)) continue;

        uint32_t raw_seq = 0;
        std::memcpy(&raw_seq, raw_buf, sizeof(raw_seq));
        const uint32_t seq = ntohl(raw_seq);
        
        if ((seq & ~NetParams::ValidSeqBits) != 0) continue;

        for (std::size_t row_idx = 0; row_idx < NetParams::TotalChunks; ++row_idx) {
            const uint16_t stream_id = htons(static_cast<uint16_t>(
                (static_cast<uint16_t>(row_idx) << 11u) | seq
            ));
            std::memcpy(encoded_buf, &stream_id, sizeof(stream_id));

            for (std::size_t b_idx = 0; b_idx < NetParams::ChunkLen; ++b_idx) {
                uint8_t fec_byte = 0;
                for (std::size_t col_idx = 0; col_idx < NetParams::MinChunks; ++col_idx) {
                    const std::size_t map_idx = col_idx * NetParams::ChunkLen + b_idx;
                    const uint8_t src_byte = raw_buf[NetParams::RawHdrSize + map_idx];
                    fec_byte ^= galois_multiply(generator.data[row_idx][col_idx], src_byte);
                }
                encoded_buf[NetParams::EncHdrLen + b_idx] = fec_byte;
            }
            
            if (!transmit_chunk(sender_fd, remote_dest, encoded_buf)) {
                std::perror("sendto(relay)");
            }
        }
    }

    close(sender_fd);
    close(listener_fd);
    return 0;
}
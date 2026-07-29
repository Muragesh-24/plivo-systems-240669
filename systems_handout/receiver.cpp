#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace FecConfig {
    constexpr uint16_t kIngressPort = 47002;
    constexpr uint16_t kEgressPort = 47020;
    constexpr std::size_t kDataCapacity = 160;
    constexpr std::size_t kEgressPktSize = 164;
    constexpr std::size_t kRequiredSlices = 20;
    constexpr std::size_t kMaxSlices = 32;
    constexpr std::size_t kSliceSize = 8;
    constexpr std::size_t kHeaderLen = 2;
    constexpr std::size_t kIngressPktSize = kHeaderLen + kSliceSize;
    constexpr uint32_t kSeqMaskBits = 0x07ffu;
}

using SliceData = std::array<uint8_t, FecConfig::kSliceSize>;

struct BlockBuffer {
    std::array<SliceData, FecConfig::kMaxSlices> slices{};
    std::array<bool, FecConfig::kMaxSlices> has_slice{};
    uint8_t slice_count = 0;
    bool is_completed = false;
};

uint8_t galois_mul(uint8_t lhs, uint8_t rhs) {
    uint8_t out = 0;
    for (; rhs != 0; rhs >>= 1u) {
        if (rhs & 1u) {
            out ^= lhs;
        }
        const bool msb = (lhs & 0x80u) != 0;
        lhs = static_cast<uint8_t>(lhs << 1u);
        if (msb) {
            lhs ^= 0x1du;
        }
    }
    return out;
}

uint8_t galois_exp(uint8_t base, unsigned pwr) {
    uint8_t out = 1;
    for (; pwr != 0; pwr >>= 1u) {
        if (pwr & 1u) {
            out = galois_mul(out, base);
        }
        base = galois_mul(base, base);
    }
    return out;
}

std::size_t calculate_max_frames() {
    const char* env_dur = std::getenv("DURATION_S");
    if (!env_dur) return 1500;
    
    char* tail = nullptr;
    const double dur_secs = std::strtod(env_dur, &tail);
    if (tail == env_dur || dur_secs <= 0.0) return 1500;
    
    return static_cast<std::size_t>(dur_secs * 50.0);
}

bool recover_payload(const BlockBuffer& block, std::array<uint8_t, FecConfig::kDataCapacity>& out_payload) {
    uint8_t aug_matrix[FecConfig::kRequiredSlices][FecConfig::kRequiredSlices + FecConfig::kSliceSize]{};
    std::size_t current_row = 0;
    
    for (std::size_t idx = 0; idx < FecConfig::kMaxSlices && current_row < FecConfig::kRequiredSlices; ++idx) {
        if (!block.has_slice[idx]) continue;
        
        const uint8_t x_val = static_cast<uint8_t>(idx + 1);
        aug_matrix[current_row][0] = 1;
        
        for (std::size_t col = 1; col < FecConfig::kRequiredSlices; ++col) {
            aug_matrix[current_row][col] = galois_mul(aug_matrix[current_row][col - 1], x_val);
        }
        
        std::memcpy(aug_matrix[current_row] + FecConfig::kRequiredSlices, block.slices[idx].data(), FecConfig::kSliceSize);
        ++current_row;
    }
    
    if (current_row != FecConfig::kRequiredSlices) return false;

    // Apply Gauss-Jordan elimination
    for (std::size_t col = 0; col < FecConfig::kRequiredSlices; ++col) {
        std::size_t pivot_idx = col;
        while (pivot_idx < FecConfig::kRequiredSlices && aug_matrix[pivot_idx][col] == 0) {
            ++pivot_idx;
        }
        
        if (pivot_idx == FecConfig::kRequiredSlices) return false;
        
        if (pivot_idx != col) {
            for (std::size_t k = 0; k < FecConfig::kRequiredSlices + FecConfig::kSliceSize; ++k) {
                std::swap(aug_matrix[pivot_idx][k], aug_matrix[col][k]);
            }
        }

        const uint8_t inv_val = galois_exp(aug_matrix[col][col], 254);
        for (std::size_t k = col; k < FecConfig::kRequiredSlices + FecConfig::kSliceSize; ++k) {
            aug_matrix[col][k] = galois_mul(aug_matrix[col][k], inv_val);
        }

        for (std::size_t r = 0; r < FecConfig::kRequiredSlices; ++r) {
            if (r == col) continue;
            
            const uint8_t multiplier = aug_matrix[r][col];
            if (multiplier == 0) continue;
            
            for (std::size_t k = col; k < FecConfig::kRequiredSlices + FecConfig::kSliceSize; ++k) {
                aug_matrix[r][k] ^= galois_mul(multiplier, aug_matrix[col][k]);
            }
        }
    }

    // Unpack data
    for (std::size_t r = 0; r < FecConfig::kRequiredSlices; ++r) {
        for (std::size_t b = 0; b < FecConfig::kSliceSize; ++b) {
            const std::size_t target_idx = r * FecConfig::kSliceSize + b;
            if (target_idx < FecConfig::kDataCapacity) {
                out_payload[target_idx] = aug_matrix[r][FecConfig::kRequiredSlices + b];
            }
        }
    }
    
    return true;
}

}  // namespace

int main() {
    const std::size_t total_frames = calculate_max_frames();
    if (total_frames == 0 || total_frames > FecConfig::kSeqMaskBits + 1u) return 1;

    int sock_in = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_in < 0) {
        std::perror("socket(ingress)");
        return 1;
    }
    
    sockaddr_in rx_addr{};
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_port = htons(FecConfig::kIngressPort);
    rx_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    if (bind(sock_in, reinterpret_cast<sockaddr*>(&rx_addr), sizeof(rx_addr)) < 0) {
        std::perror("bind(rx_addr)");
        close(sock_in);
        return 1;
    }

    int sock_out = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_out < 0) {
        std::perror("socket(egress)");
        close(sock_in);
        return 1;
    }
    
    sockaddr_in tx_addr{};
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_port = htons(FecConfig::kEgressPort);
    tx_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::vector<BlockBuffer> buffers(total_frames);
    unsigned char net_buffer[65536];
    
    for (;;) {
        const ssize_t rx_bytes = recvfrom(sock_in, net_buffer, sizeof(net_buffer), 0, nullptr, nullptr);
        
        if (rx_bytes < 0) {
            if (errno == EINTR) continue;
            std::perror("recvfrom(ingress)");
            break;
        }
        
        if (rx_bytes != static_cast<ssize_t>(FecConfig::kIngressPktSize)) continue;

        uint16_t raw_id = 0;
        std::memcpy(&raw_id, net_buffer, sizeof(raw_id));
        const uint16_t parsed_id = ntohs(raw_id);
        
        const std::size_t slice_idx = parsed_id >> 11u;
        const std::size_t seq_num = parsed_id & FecConfig::kSeqMaskBits;
        
        if (seq_num >= total_frames || slice_idx >= FecConfig::kMaxSlices) continue;

        BlockBuffer& current_block = buffers[seq_num];
        if (current_block.is_completed || current_block.has_slice[slice_idx]) continue;
        
        std::memcpy(current_block.slices[slice_idx].data(), net_buffer + FecConfig::kHeaderLen, FecConfig::kSliceSize);
        current_block.has_slice[slice_idx] = true;
        ++current_block.slice_count;
        
        if (current_block.slice_count < FecConfig::kRequiredSlices) continue;

        std::array<uint8_t, FecConfig::kDataCapacity> recovered_data{};
        if (!recover_payload(current_block, recovered_data)) continue;
        
        current_block.is_completed = true;

        unsigned char egress_pkt[FecConfig::kEgressPktSize];
        const uint32_t net_seq = htonl(static_cast<uint32_t>(seq_num));
        
        std::memcpy(egress_pkt, &net_seq, sizeof(net_seq));
        std::memcpy(egress_pkt + 4, recovered_data.data(), FecConfig::kDataCapacity);
        
        ssize_t tx_bytes;
        do {
            tx_bytes = sendto(sock_out, egress_pkt, sizeof(egress_pkt), 0,
                              reinterpret_cast<const sockaddr*>(&tx_addr), sizeof(tx_addr));
        } while (tx_bytes < 0 && errno == EINTR);
        
        if (tx_bytes != static_cast<ssize_t>(sizeof(egress_pkt))) {
            std::perror("sendto(egress)");
        }
    }

    close(sock_out);
    close(sock_in);
    return 0;
}
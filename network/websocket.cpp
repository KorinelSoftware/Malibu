// network/websocket.cpp
// RFC 6455 frame codec + opening-handshake accept (SHA-1 + base64 via OpenSSL).

#include "malibu/network/websocket.h"

#include <openssl/evp.h>

#include <cstring>

namespace malibu::network {
namespace {
constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void put_mask(std::vector<uint8_t>& out, uint64_t& counter) {
    // Deterministic but non-zero masking key (a real client uses CSPRNG bytes).
    for (int i = 0; i < 4; ++i) {
        counter = counter * 6364136223846793005ULL + 1442695040888963407ULL;
        out.push_back(static_cast<uint8_t>(counter >> 33));
    }
}
}  // namespace

std::vector<uint8_t> ws_encode_frame(WSFrameType type, const uint8_t* data, size_t len,
                                     bool fin, const uint8_t mask[4]) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | (static_cast<uint8_t>(type) & 0x0F)));

    uint8_t mask_bit = mask ? 0x80 : 0x00;
    if (len < 126) {
        out.push_back(static_cast<uint8_t>(mask_bit | len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<uint8_t>(mask_bit | 126));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        out.push_back(static_cast<uint8_t>(mask_bit | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }
    if (mask) {
        out.insert(out.end(), mask, mask + 4);
        for (size_t i = 0; i < len; ++i) out.push_back(static_cast<uint8_t>(data[i] ^ mask[i % 4]));
    } else {
        out.insert(out.end(), data, data + len);
    }
    return out;
}

WSDecode ws_decode_frame(const uint8_t* buf, size_t len, WSFrame& out, size_t& consumed) {
    if (len < 2) return WSDecode::NeedMore;
    bool fin = (buf[0] & 0x80) != 0;
    uint8_t opcode = buf[0] & 0x0F;
    bool masked = (buf[1] & 0x80) != 0;
    uint64_t payload_len = buf[1] & 0x7F;
    size_t pos = 2;

    if (payload_len == 126) {
        if (len < 4) return WSDecode::NeedMore;
        payload_len = (static_cast<uint64_t>(buf[2]) << 8) | buf[3];
        pos = 4;
    } else if (payload_len == 127) {
        if (len < 10) return WSDecode::NeedMore;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) payload_len = (payload_len << 8) | buf[2 + i];
        pos = 10;
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (len < pos + 4) return WSDecode::NeedMore;
        std::memcpy(mask, buf + pos, 4);
        pos += 4;
    }
    if (len < pos + payload_len) return WSDecode::NeedMore;

    out.fin = fin;
    out.masked = masked;
    out.type = static_cast<WSFrameType>(opcode);
    out.payload.resize(payload_len);
    for (uint64_t i = 0; i < payload_len; ++i) {
        uint8_t b = buf[pos + i];
        out.payload[i] = masked ? static_cast<uint8_t>(b ^ mask[i % 4]) : b;
    }
    consumed = pos + payload_len;
    return WSDecode::Ok;
}

std::string ws_compute_accept(const std::string& key) {
    std::string concat = key + kGuid;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, concat.data(), concat.size());
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    // base64 of the 20-byte SHA-1 digest.
    char b64[64];
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64), digest, static_cast<int>(digest_len));
    return std::string(b64, static_cast<size_t>(n));
}

// ---- WebSocket framing state machine ----
std::vector<uint8_t> WebSocket::text_frame(std::string_view text, bool mask) {
    std::vector<uint8_t> mk;
    if (mask) put_mask(mk, mask_counter_);
    return ws_encode_frame(WSFrameType::Text, reinterpret_cast<const uint8_t*>(text.data()),
                           text.size(), true, mask ? mk.data() : nullptr);
}
std::vector<uint8_t> WebSocket::binary_frame(const std::vector<uint8_t>& data, bool mask) {
    std::vector<uint8_t> mk;
    if (mask) put_mask(mk, mask_counter_);
    return ws_encode_frame(WSFrameType::Binary, data.data(), data.size(), true, mask ? mk.data() : nullptr);
}
std::vector<uint8_t> WebSocket::ping_frame(const std::vector<uint8_t>& data, bool mask) {
    std::vector<uint8_t> mk;
    if (mask) put_mask(mk, mask_counter_);
    return ws_encode_frame(WSFrameType::Ping, data.data(), data.size(), true, mask ? mk.data() : nullptr);
}
std::vector<uint8_t> WebSocket::pong_frame(const std::vector<uint8_t>& data, bool mask) {
    std::vector<uint8_t> mk;
    if (mask) put_mask(mk, mask_counter_);
    return ws_encode_frame(WSFrameType::Pong, data.data(), data.size(), true, mask ? mk.data() : nullptr);
}
std::vector<uint8_t> WebSocket::close_frame(uint16_t code, std::string_view reason, bool mask) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    payload.insert(payload.end(), reason.begin(), reason.end());
    std::vector<uint8_t> mk;
    if (mask) put_mask(mk, mask_counter_);
    return ws_encode_frame(WSFrameType::Close, payload.data(), payload.size(), true, mask ? mk.data() : nullptr);
}

std::vector<WebSocket::Message> WebSocket::feed(const uint8_t* data, size_t len) {
    rx_buffer_.insert(rx_buffer_.end(), data, data + len);
    std::vector<Message> messages;

    size_t offset = 0;
    for (;;) {
        WSFrame frame;
        size_t consumed = 0;
        WSDecode r = ws_decode_frame(rx_buffer_.data() + offset, rx_buffer_.size() - offset, frame, consumed);
        if (r != WSDecode::Ok) break;
        offset += consumed;

        bool control = frame.type == WSFrameType::Close || frame.type == WSFrameType::Ping ||
                       frame.type == WSFrameType::Pong;
        if (control) {
            messages.push_back(Message{frame.type, frame.payload});
            continue;
        }
        if (frame.type == WSFrameType::Continuation) {
            frag_data_.insert(frag_data_.end(), frame.payload.begin(), frame.payload.end());
        } else {
            frag_type_ = frame.type;
            frag_data_ = frame.payload;
            fragmenting_ = true;
        }
        if (frame.fin && fragmenting_) {
            messages.push_back(Message{frag_type_, frag_data_});
            fragmenting_ = false;
            frag_data_.clear();
        }
    }
    if (offset > 0) rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + offset);
    return messages;
}

} // namespace malibu::network

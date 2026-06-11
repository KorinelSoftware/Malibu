#pragma once
// core/include/malibu/network/websocket.h
// WebSocket protocol per RFC 6455: frame codec (text/binary/ping/pong/close,
// masking, fragmentation) and the opening-handshake accept computation. The
// byte transport (TCP/TLS) is delegated to the platform; this is the protocol.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace malibu::network {

enum class WSFrameType : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct WSFrame {
    WSFrameType          type = WSFrameType::Text;
    bool                 fin = true;
    bool                 masked = false;
    std::vector<uint8_t> payload;
};

enum class WSDecode : uint8_t { Ok, NeedMore, Error };

// Encodes a single frame. If `mask` is non-null (client→server), the 4-byte key
// masks the payload per RFC 6455 §5.3.
std::vector<uint8_t> ws_encode_frame(WSFrameType type, const uint8_t* data, size_t len,
                                     bool fin = true, const uint8_t mask[4] = nullptr);

// Decodes one frame from `buf`. On Ok, `consumed` is the number of bytes used.
WSDecode ws_decode_frame(const uint8_t* buf, size_t len, WSFrame& out, size_t& consumed);

// Sec-WebSocket-Accept = base64(SHA1(key + RFC6455 GUID)).
std::string ws_compute_accept(const std::string& sec_websocket_key);

// A framing state machine: build outgoing frames and reassemble incoming
// messages (handling fragmentation) from a byte stream.
class WebSocket {
public:
    struct Message {
        WSFrameType          type;
        std::vector<uint8_t> data;
    };

    // Outgoing frames (client side masks by default per RFC 6455).
    std::vector<uint8_t> text_frame(std::string_view text, bool mask = true);
    std::vector<uint8_t> binary_frame(const std::vector<uint8_t>& data, bool mask = true);
    std::vector<uint8_t> ping_frame(const std::vector<uint8_t>& data = {}, bool mask = true);
    std::vector<uint8_t> pong_frame(const std::vector<uint8_t>& data = {}, bool mask = true);
    std::vector<uint8_t> close_frame(uint16_t code = 1000, std::string_view reason = "", bool mask = true);

    // Feeds received bytes; returns any complete messages (fragments reassembled).
    std::vector<Message> feed(const uint8_t* data, size_t len);

private:
    std::vector<uint8_t>  rx_buffer_;
    bool                  fragmenting_ = false;
    WSFrameType           frag_type_ = WSFrameType::Text;
    std::vector<uint8_t>  frag_data_;
    uint64_t              mask_counter_ = 0x12345678u;  // deterministic mask source
};

} // namespace malibu::network

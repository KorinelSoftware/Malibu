// tests/test_websocket.cpp
// Task 23: WebSocket protocol (RFC 6455) — handshake accept, frame codec,
// masking, fragmentation, control frames, extended lengths.

#include <gtest/gtest.h>
#include "malibu/network/websocket.h"

#include <string>

using namespace malibu::network;

TEST(WebSocket, AcceptMatchesRfcExample) {
    // RFC 6455 §1.3 worked example.
    EXPECT_EQ(ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocket, EncodeDecodeUnmasked) {
    std::string msg = "hello";
    auto bytes = ws_encode_frame(WSFrameType::Text, reinterpret_cast<const uint8_t*>(msg.data()),
                                 msg.size(), true, nullptr);
    WSFrame f; size_t consumed = 0;
    ASSERT_EQ(ws_decode_frame(bytes.data(), bytes.size(), f, consumed), WSDecode::Ok);
    EXPECT_EQ(consumed, bytes.size());
    EXPECT_EQ(f.type, WSFrameType::Text);
    EXPECT_TRUE(f.fin);
    EXPECT_FALSE(f.masked);
    EXPECT_EQ(std::string(f.payload.begin(), f.payload.end()), "hello");
}

TEST(WebSocket, EncodeDecodeMasked) {
    std::string msg = "masked payload";
    uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    auto bytes = ws_encode_frame(WSFrameType::Text, reinterpret_cast<const uint8_t*>(msg.data()),
                                 msg.size(), true, mask);
    EXPECT_TRUE(bytes[1] & 0x80);  // MASK bit set
    WSFrame f; size_t consumed = 0;
    ASSERT_EQ(ws_decode_frame(bytes.data(), bytes.size(), f, consumed), WSDecode::Ok);
    EXPECT_TRUE(f.masked);
    EXPECT_EQ(std::string(f.payload.begin(), f.payload.end()), "masked payload");
}

TEST(WebSocket, DecodeNeedsMore) {
    std::string msg = "data";
    auto bytes = ws_encode_frame(WSFrameType::Text, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    WSFrame f; size_t consumed = 0;
    EXPECT_EQ(ws_decode_frame(bytes.data(), 1, f, consumed), WSDecode::NeedMore);  // partial header
}

TEST(WebSocket, ExtendedLength16And64) {
    std::vector<uint8_t> big(1000, 0x5a);   // > 125 → 16-bit length
    auto b16 = ws_encode_frame(WSFrameType::Binary, big.data(), big.size());
    EXPECT_EQ(b16[1] & 0x7F, 126);
    WSFrame f; size_t consumed = 0;
    ASSERT_EQ(ws_decode_frame(b16.data(), b16.size(), f, consumed), WSDecode::Ok);
    EXPECT_EQ(f.payload.size(), 1000u);

    std::vector<uint8_t> huge(70000, 0x5a);  // > 65535 → 64-bit length
    auto b64 = ws_encode_frame(WSFrameType::Binary, huge.data(), huge.size());
    EXPECT_EQ(b64[1] & 0x7F, 127);
    ASSERT_EQ(ws_decode_frame(b64.data(), b64.size(), f, consumed), WSDecode::Ok);
    EXPECT_EQ(f.payload.size(), 70000u);
}

TEST(WebSocket, RoundTripThroughStateMachine) {
    WebSocket client, server;
    auto wire = client.text_frame("ping from client", true);
    auto msgs = server.feed(wire.data(), wire.size());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].type, WSFrameType::Text);
    EXPECT_EQ(std::string(msgs[0].data.begin(), msgs[0].data.end()), "ping from client");
}

TEST(WebSocket, ReassemblesFragments) {
    // Two frames: text(FIN=0) "Hel" + continuation(FIN=1) "lo".
    std::string a = "Hel", b = "lo";
    auto f1 = ws_encode_frame(WSFrameType::Text, reinterpret_cast<const uint8_t*>(a.data()), a.size(), false);
    auto f2 = ws_encode_frame(WSFrameType::Continuation, reinterpret_cast<const uint8_t*>(b.data()), b.size(), true);
    WebSocket ws;
    auto m1 = ws.feed(f1.data(), f1.size());
    EXPECT_TRUE(m1.empty());  // not complete yet
    auto m2 = ws.feed(f2.data(), f2.size());
    ASSERT_EQ(m2.size(), 1u);
    EXPECT_EQ(std::string(m2[0].data.begin(), m2[0].data.end()), "Hello");
}

TEST(WebSocket, ControlFramesSurfaced) {
    WebSocket peer;
    WebSocket sender;
    auto ping = sender.ping_frame({0x01, 0x02}, false);
    auto msgs = peer.feed(ping.data(), ping.size());
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].type, WSFrameType::Ping);

    auto close = sender.close_frame(1000, "bye", false);
    auto cm = peer.feed(close.data(), close.size());
    ASSERT_EQ(cm.size(), 1u);
    EXPECT_EQ(cm[0].type, WSFrameType::Close);
}

TEST(WebSocket, PartialThenCompleteDelivery) {
    WebSocket ws;
    auto wire = ws.text_frame("streamed", false);
    // Feed one byte at a time; the message only appears once fully buffered.
    std::vector<WebSocket::Message> msgs;
    for (size_t i = 0; i < wire.size(); ++i) {
        auto m = ws.feed(&wire[i], 1);
        for (auto& x : m) msgs.push_back(x);
    }
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(std::string(msgs[0].data.begin(), msgs[0].data.end()), "streamed");
}

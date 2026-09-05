#include "Common.h"
#include <eacp/Network/WebSocket/Protocol.h>

#include <vector>

using namespace nano;
using eacp::WebSocket::CloseStatus;
using eacp::WebSocket::Protocol::acceptKeyFor;
using eacp::WebSocket::Protocol::decode;
using eacp::WebSocket::Protocol::decodeClose;
using eacp::WebSocket::Protocol::encode;
using eacp::WebSocket::Protocol::encodeClose;
using eacp::WebSocket::Protocol::Error;
using eacp::WebSocket::Protocol::Frame;
using eacp::WebSocket::Protocol::Opcode;

namespace
{
std::string protocolPayloadOf(std::size_t size)
{
    auto payload = std::string(size, '\0');

    for (auto i = std::size_t {0}; i < size; ++i)
        payload[i] = (char) (i % 251);

    return payload;
}

std::vector<std::size_t> protocolPayloadSizes()
{
    return {0, 1, 125, 126, 127, 65535, 65536, 200000};
}

std::vector<Opcode> protocolOpcodes()
{
    return {Opcode::continuation,
            Opcode::text,
            Opcode::binary,
            Opcode::close,
            Opcode::ping,
            Opcode::pong};
}

bool protocolIsControl(Opcode opcode)
{
    return ((std::uint8_t) opcode & 0x08) != 0;
}

bool protocolRoundTrips(const Frame& frame, bool masked)
{
    auto wire = encode(frame, masked);
    auto decoded = decode(wire);

    return decoded.has_value() && decoded->consumed == wire.size()
           && decoded->frame.opcode == frame.opcode
           && decoded->frame.fin == frame.fin
           && decoded->frame.payload == frame.payload;
}

bool protocolThrowsOn(std::string_view wire)
{
    try
    {
        decode(wire);
    }
    catch (const Error&)
    {
        return true;
    }

    return false;
}
} // namespace

auto tAcceptKeyVector = test("WebSocketProtocol/acceptKeyMatchesRfc6455") = []
{
    // RFC 6455 §1.3's worked example, which is also the SHA-1 and base64
    // implementations' only vector that matters.
    check(acceptKeyFor("dGhlIHNhbXBsZSBub25jZQ==")
          == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
};

auto tAcceptKeyOtherVectors =
    test("WebSocketProtocol/acceptKeyHashesKnownInputs") = []
{
    // Independently computed against a reference SHA-1: the guarantee is that
    // the digest is right for inputs that straddle the 64-byte block boundary.
    check(acceptKeyFor("x3JJHMbDL1EzLkh9GBhXDw==")
          == "HSmrc0sMlYUkAGmm5OPpG2HaGWk=");
    check(acceptKeyFor("AAAAAAAAAAAAAAAAAAAAAA==")
          == "ICX+Yqv66kxgM0FcWaLWlFLwTAI=");
    check(acceptKeyFor("") == "Kfh9QIsMVZcl6xEPYxPHzW8SZ8w=");
};

auto tRoundTripSizes = test("WebSocketProtocol/roundTripsEverySizeAndOpcode") = []
{
    auto ok = true;

    for (auto opcode: protocolOpcodes())
    {
        for (auto size: protocolPayloadSizes())
        {
            if (protocolIsControl(opcode) && size > 125)
                continue;

            auto frame = Frame {opcode, true, protocolPayloadOf(size)};

            ok = ok && protocolRoundTrips(frame, false);
            ok = ok && protocolRoundTrips(frame, true);
        }
    }

    check(ok);
};

auto tRoundTripFragments = test("WebSocketProtocol/roundTripsANonFinalFrame") = []
{
    auto frame = Frame {Opcode::text, false, "half a message"};

    check(protocolRoundTrips(frame, false));
    check(protocolRoundTrips(frame, true));
};

auto tLengthForms = test("WebSocketProtocol/picksTheShortestLengthForm") = []
{
    check(encode({Opcode::text, true, std::string(125, 'a')}).size() == 125 + 2);
    check(encode({Opcode::text, true, std::string(126, 'a')}).size() == 126 + 4);
    check(encode({Opcode::text, true, std::string(65535, 'a')}).size() == 65535 + 4);
    check(encode({Opcode::text, true, std::string(65536, 'a')}).size()
          == 65536 + 10);
};

auto tMaskingChangesTheBytes =
    test("WebSocketProtocol/aMaskedFrameCarriesTheKeyAndHidesThePayload") = []
{
    auto payload = std::string(64, 'A');
    auto masked = encode({Opcode::binary, true, payload}, true);

    check(((std::uint8_t) masked[1] & 0x80) != 0);
    check(masked.size() == payload.size() + 2 + 4);
    check(masked.substr(6) != payload);
    check(decode(masked)->frame.payload == payload);
};

auto tTruncatedNeverThrows =
    test("WebSocketProtocol/aTruncatedFrameIsIncompleteAtEveryPrefix") = []
{
    auto ok = true;

    for (auto size: std::vector<std::size_t> {0, 125, 126, 65536})
    {
        for (auto masked: {false, true})
        {
            auto wire =
                encode({Opcode::binary, true, protocolPayloadOf(size)}, masked);

            for (auto prefix = std::size_t {0}; prefix < wire.size(); ++prefix)
            {
                auto decoded = std::optional<eacp::WebSocket::Protocol::Decoded>();

                try
                {
                    decoded = decode(std::string_view(wire).substr(0, prefix));
                }
                catch (const Error&)
                {
                    ok = false;
                }

                ok = ok && !decoded.has_value();
            }
        }
    }

    check(ok);
};

auto tTrailingBytesAreLeft =
    test("WebSocketProtocol/decodeConsumesOneFrameAndLeavesTheRest") = []
{
    auto first = encode({Opcode::text, true, "one"});
    auto second = encode({Opcode::text, true, "two"});

    auto decoded = decode(first + second);

    check(decoded.has_value());
    check(decoded->consumed == first.size());
    check(decoded->frame.payload == "one");

    auto next = decode((first + second).substr(decoded->consumed));

    check(next.has_value());
    check(next->frame.payload == "two");
};

auto tMalformedHeaders = test("WebSocketProtocol/refusesHeadersNoPeerMaySend") = []
{
    check(protocolThrowsOn(std::string("\x40\x00", 2))); // RSV1 set
    check(protocolThrowsOn(std::string("\x20\x00", 2))); // RSV2 set
    check(protocolThrowsOn(std::string("\x10\x00", 2))); // RSV3 set
    check(protocolThrowsOn(std::string("\x83\x00", 2))); // opcode 3
    check(protocolThrowsOn(std::string("\x8B\x00", 2))); // opcode B
    check(protocolThrowsOn(std::string("\x08\x00", 2))); // fragmented close
    check(protocolThrowsOn(std::string("\x89\x7E", 2))); // ping of 126 bytes
    check(protocolThrowsOn(std::string("\x8A\x7F", 2))); // pong of 127+ bytes
};

auto tOversizedLength =
    test("WebSocketProtocol/refusesALengthWithItsHighBitSet") = []
{
    auto header =
        std::string("\x82\x7F", 2) + std::string("\xFF", 1) + std::string(7, '\0');

    check(protocolThrowsOn(header));
};

auto tCloseRoundTrip = test("WebSocketProtocol/closePayloadRoundTrips") = []
{
    auto encoded = encodeClose(1000, "done");
    auto status = decodeClose(encoded);

    check(encoded.size() == 6);
    check((std::uint8_t) encoded[0] == 0x03);
    check((std::uint8_t) encoded[1] == 0xE8);
    check(status.code == 1000);
    check(status.reason == "done");
};

auto tCloseEmpty = test("WebSocketProtocol/anEmptyClosePayloadIs1005") = []
{
    check(decodeClose("").code == 1005);
    check(decodeClose("").reason.empty());
    check(encodeClose(1005, "ignored").empty());
};

auto tCloseUtf8Reason = test("WebSocketProtocol/closeCarriesAUtf8Reason") = []
{
    auto reason = std::string("adiós — 見えない");
    auto status = decodeClose(encodeClose(4000, reason));

    check(status.code == 4000);
    check(status.reason == reason);
};

auto tCloseApplicationCodes =
    test("WebSocketProtocol/closeCarriesApplicationCodes") = []
{
    check(decodeClose(encodeClose(4999, "")).code == 4999);
    check(decodeClose(encodeClose(1001, "going away")).reason == "going away");
};

auto tCloseSingleByte = test("WebSocketProtocol/aOneBytePayloadIsRefused") = []
{
    auto threw = false;

    try
    {
        decodeClose(std::string("\x03", 1));
    }
    catch (const Error&)
    {
        threw = true;
    }

    check(threw);
};

auto tCloseThroughAFrame = test("WebSocketProtocol/aCloseFrameCarriesItsStatus") = []
{
    auto frame = Frame {Opcode::close, true, encodeClose(1001, "going away")};
    auto decoded = decode(encode(frame, true));

    check(decoded.has_value());
    check(decoded->frame.opcode == Opcode::close);

    auto status = decodeClose(decoded->frame.payload);

    check(status.code == 1001);
    check(status.reason == "going away");
};

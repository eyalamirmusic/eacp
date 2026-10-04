#include "Common.h"
#include <eacp/Network/WebSocket/Protocol.h>

using namespace nano;
using eacp::Vector;
using eacp::WebSocket::CloseStatus;
using eacp::WebSocket::Protocol::acceptKeyFor;
using eacp::WebSocket::Protocol::clientHandshakeRequest;
using eacp::WebSocket::Protocol::decode;
using eacp::WebSocket::Protocol::decodeClose;
using eacp::WebSocket::Protocol::encode;
using eacp::WebSocket::Protocol::encodeClose;
using eacp::WebSocket::Protocol::Error;
using eacp::WebSocket::Protocol::Frame;
using eacp::WebSocket::Protocol::Opcode;
using eacp::WebSocket::Protocol::parseUrl;
using eacp::WebSocket::Protocol::randomClientKey;
using eacp::WebSocket::Protocol::validateHandshakeResponse;

namespace
{
std::string protocolPayloadOf(int size)
{
    auto payload = std::string((std::size_t) size, '\0');

    for (auto i = 0; i < size; ++i)
        payload[(std::size_t) i] = (char) (i % 251);

    return payload;
}

Vector<int> protocolPayloadSizes()
{
    return {0, 1, 125, 126, 127, 65535, 65536, 200000};
}

Vector<Opcode> protocolOpcodes()
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

    return decoded.has_value() && decoded->consumed == (int) wire.size()
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

constexpr auto protocolSampleKey = "dGhlIHNhbXBsZSBub25jZQ==";

std::string protocolUpgradeAnswer(const std::string& status,
                                  const std::string& acceptKey,
                                  const std::string& extraHeaders = {})
{
    return status + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           + "Sec-WebSocket-Accept: " + acceptKey + "\r\n" + extraHeaders + "\r\n";
}

std::string protocolValidAnswer(const std::string& extraHeaders = {})
{
    return protocolUpgradeAnswer("HTTP/1.1 101 Switching Protocols",
                                 acceptKeyFor(protocolSampleKey),
                                 extraHeaders);
}

bool protocolContains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
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

    for (auto size: Vector<int> {0, 125, 126, 65536})
    {
        for (auto masked: {false, true})
        {
            auto wire =
                encode({Opcode::binary, true, protocolPayloadOf(size)}, masked);

            for (auto prefix = 0; prefix < (int) wire.size(); ++prefix)
            {
                auto decoded = std::optional<eacp::WebSocket::Protocol::Decoded>();

                try
                {
                    decoded = decode(
                        std::string_view(wire).substr(0, (std::size_t) prefix));
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
    check(decoded->consumed == (int) first.size());
    check(decoded->frame.payload == "one");

    auto next = decode((first + second).substr((std::size_t) decoded->consumed));

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

auto tParseUrlParts = test("WebSocketProtocol/parseUrlSplitsAWsUrl") = []
{
    auto plain = parseUrl("ws://example.com/chat?room=1#top");

    check(plain.has_value());
    check(!plain->secure);
    check(plain->host == "example.com");
    check(plain->port == 80);
    check(plain->target == "/chat?room=1");

    auto secure = parseUrl("WSS://user@example.com:8443");

    check(secure.has_value());
    check(secure->secure);
    check(secure->host == "example.com");
    check(secure->port == 8443);
    check(secure->target == "/");

    auto query = parseUrl("ws://127.0.0.1:9?x=1");

    check(query.has_value());
    check(query->port == 9);
    check(query->target == "/?x=1");

    auto literal = parseUrl("wss://[::1]:9000/socket");

    check(literal.has_value());
    check(literal->host == "::1");
    check(literal->port == 9000);
    check(parseUrl("wss://[::1]/")->port == 443);
};

auto tParseUrlRefuses = test("WebSocketProtocol/parseUrlRefusesWhatIsNotWs") = []
{
    check(!parseUrl("http://example.com/").has_value());
    check(!parseUrl("example.com").has_value());
    check(!parseUrl("ws:///path").has_value());
    check(!parseUrl("ws://host:0/").has_value());
    check(!parseUrl("ws://host:65536/").has_value());
    check(!parseUrl("ws://host:12ab/").has_value());
    check(!parseUrl("ws://[::1/").has_value());
};

auto tRandomKey = test("WebSocketProtocol/aClientKeyIsSixteenRandomBytes") = []
{
    auto key = randomClientKey();
    auto decoded = eacp::Base64::decode(key);

    check(key.size() == 24);
    check(decoded.has_value() && decoded->size() == 16);
    check(randomClientKey() != key);
};

auto tRequestShape =
    test("WebSocketProtocol/theUpgradeRequestCarriesTheHandshake") = []
{
    auto options = eacp::WebSocket::Options();
    options.protocols.add("chat");
    options.protocols.add("superchat");
    options.headers["Origin"] = "https://eacp.test";
    options.headers["Sec-WebSocket-Key"] = "forged";
    options.headers["X-Injected"] = "a\r\nEvil: yes";

    auto request = clientHandshakeRequest(
        *parseUrl("ws://example.com:8080/chat?a=b"), protocolSampleKey, options);

    check(request.starts_with("GET /chat?a=b HTTP/1.1\r\n"));
    check(request.ends_with("\r\n\r\n"));
    check(protocolContains(request, "\r\nHost: example.com:8080\r\n"));
    check(protocolContains(request, "\r\nUpgrade: websocket\r\n"));
    check(protocolContains(request, "\r\nConnection: Upgrade\r\n"));
    check(protocolContains(request, "\r\nSec-WebSocket-Version: 13\r\n"));
    check(protocolContains(request,
                           std::string("\r\nSec-WebSocket-Key: ") + protocolSampleKey
                               + "\r\n"));
    check(protocolContains(request,
                           "\r\nSec-WebSocket-Protocol: chat, superchat\r\n"));
    check(protocolContains(request, "\r\nOrigin: https://eacp.test\r\n"));
    check(!protocolContains(request, "forged"));
    check(!protocolContains(request, "Evil"));
};

auto tRequestHostPort =
    test("WebSocketProtocol/theHostNamesThePortOnlyWhenNeeded") = []
{
    auto options = eacp::WebSocket::Options();

    auto hostOf = [&options](std::string_view url)
    { return clientHandshakeRequest(*parseUrl(url), "k", options); };

    check(protocolContains(hostOf("ws://a.test/"), "\r\nHost: a.test\r\n"));
    check(protocolContains(hostOf("ws://a.test:80/"), "\r\nHost: a.test\r\n"));
    check(protocolContains(hostOf("wss://a.test:443/"), "\r\nHost: a.test\r\n"));
    check(protocolContains(hostOf("wss://a.test:80/"), "\r\nHost: a.test:80\r\n"));
    check(protocolContains(hostOf("ws://a.test:443/"), "\r\nHost: a.test:443\r\n"));
    check(protocolContains(hostOf("ws://[::1]:9/"), "\r\nHost: [::1]:9\r\n"));
    check(!protocolContains(hostOf("ws://a.test/"), "Sec-WebSocket-Protocol"));
};

auto tValidAnswer = test("WebSocketProtocol/aValidUpgradeAnswerIsAccepted") = []
{
    auto offered = Vector<std::string> {"chat", "superchat"};

    auto plain =
        validateHandshakeResponse(protocolValidAnswer(), protocolSampleKey, {});

    check(plain.ok);
    check(plain.error.empty());
    check(plain.protocol.empty());

    auto chosen = validateHandshakeResponse(
        protocolValidAnswer("Sec-WebSocket-Protocol: superchat\r\n"),
        protocolSampleKey,
        offered);

    check(chosen.ok);
    check(chosen.protocol == "superchat");
};

auto tAnswerIgnoresCase =
    test("WebSocketProtocol/theUpgradeAnswerIgnoresHeaderCase") = []
{
    auto answer = std::string("HTTP/1.1 101 Switching Protocols\r\n"
                              "upgrade: WebSocket\r\n"
                              "CONNECTION: keep-alive, upgrade\r\n"
                              "sec-websocket-accept: ")
                  + acceptKeyFor(protocolSampleKey) + "\r\n\r\n";

    check(validateHandshakeResponse(answer, protocolSampleKey, {}).ok);
};

auto tWrongAccept = test("WebSocketProtocol/aWrongAcceptKeyIsRefused") = []
{
    auto answer = protocolUpgradeAnswer("HTTP/1.1 101 Switching Protocols",
                                        acceptKeyFor("another key"));
    auto result = validateHandshakeResponse(answer, protocolSampleKey, {});

    check(!result.ok);
    check(protocolContains(result.error, "Accept"));
};

auto tWrongStatus = test("WebSocketProtocol/anAnswerThatIsNot101IsRefused") = []
{
    auto notFound = protocolUpgradeAnswer("HTTP/1.1 404 Not Found",
                                          acceptKeyFor(protocolSampleKey));
    auto result = validateHandshakeResponse(notFound, protocolSampleKey, {});

    check(!result.ok);
    check(protocolContains(result.error, "404"));
    check(!validateHandshakeResponse("garbage\r\n\r\n", protocolSampleKey, {}).ok);
    check(!validateHandshakeResponse("", protocolSampleKey, {}).ok);
};

auto tMissingUpgrade =
    test("WebSocketProtocol/anAnswerThatDoesNotUpgradeIsRefused") = []
{
    auto accept = acceptKeyFor(protocolSampleKey);

    auto noUpgrade = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: "
                     + accept + "\r\n\r\n";
    auto noConnection = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                        "Sec-WebSocket-Accept: "
                        + accept + "\r\n\r\n";

    check(!validateHandshakeResponse(noUpgrade, protocolSampleKey, {}).ok);
    check(!validateHandshakeResponse(noConnection, protocolSampleKey, {}).ok);
};

auto tUnrequestedProtocol =
    test("WebSocketProtocol/aSubprotocolNobodyOfferedIsRefused") = []
{
    auto answer = protocolValidAnswer("Sec-WebSocket-Protocol: mqtt\r\n");

    auto offeredOthers =
        validateHandshakeResponse(answer, protocolSampleKey, {"chat"});
    auto offeredNone = validateHandshakeResponse(answer, protocolSampleKey, {});

    check(!offeredOthers.ok);
    check(protocolContains(offeredOthers.error, "mqtt"));
    check(!offeredNone.ok);
};

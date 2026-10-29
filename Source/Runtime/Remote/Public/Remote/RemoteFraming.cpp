#include "Remote/RemoteFraming.h"

#include "HAL/PlatformSystem.h"
#include "Memory/MemoryOps.h"
#include "String/Ascii.h"
#include "String/Number.h"
#include "String/String.h"
#include "String/StringBuilder.h"
#include "WebSocketHandshake.h"

namespace Marlin
{
RemoteFrame RemoteFrame::FromText(StringView text)
{
    RemoteFrame frame;
    frame.Bytes.ResizeUninitialized(text.Length());
    MemCopy(frame.Bytes.Data(), text.Data(), text.Length());
    return frame;
}

namespace
{
constexpr usize MaxHeaderSize = 4096;

void Append(List<uint8> &bytes, StringView text)
{
    bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(text.Data()), text.Length()));
}

/// Finds "\r\n\r\n"; the index after it, or zero.
usize FindHeaderEnd(Span<const uint8> bytes)
{
    for (usize i = 3; i < bytes.Count(); ++i)
    {
        if (bytes[i - 3] == '\r' && bytes[i - 2] == '\n' && bytes[i - 1] == '\r' && bytes[i] == '\n')
        {
            return i + 1;
        }
    }
    return 0;
}

/// Finds header *name*, case-insensitively, in an HTTP-style block.
bool FindHeader(StringView headers, StringView name, StringView &value)
{
    while (!headers.IsEmpty())
    {
        StringView line;
        StringView rest;
        if (!headers.SplitFirst('\n', line, rest))
        {
            line = headers;
            rest = {};
        }
        headers = rest;
        StringView key;
        StringView content;
        if (line.SplitFirst(':', key, content) && key.Trim().EqualsIgnoreCase(name))
        {
            value = content.Trim();
            return true;
        }
    }
    return false;
}

StringView AsText(Span<const uint8> bytes)
{
    return {reinterpret_cast<const char *>(bytes.Data()), bytes.Count()};
}

class StreamFraming final : public IRemoteFraming
{
public:
    bool Decode(Span<const uint8> bytes, usize &consumed, List<RemoteFrame> &frames, List<uint8> & /*reply*/) override
    {
        consumed = 0;
        while (true)
        {
            const Span<const uint8> rest = bytes.Slice(consumed);
            const usize headerEnd = FindHeaderEnd(rest);
            if (headerEnd == 0)
            {
                return rest.Count() <= MaxHeaderSize;
            }
            const StringView headers = AsText(rest.Slice(0, headerEnd));
            StringView lengthText;
            uint64 length = 0;
            if (!FindHeader(headers, "Content-Length", lengthText) || !TryParse(lengthText, length) ||
                length > RemoteMaxFrameSize)
            {
                return false;
            }
            if (rest.Count() < headerEnd + length)
            {
                return true;
            }
            RemoteFrame &frame = frames.AddDefaulted();
            StringView attachment;
            if (FindHeader(headers, "Marlin-Attachment", attachment) && !TryParse(attachment, frame.AttachmentId))
            {
                return false;
            }
            frame.IsAttachment = !attachment.IsEmpty();
            frame.Bytes.AddRange(rest.Slice(headerEnd, static_cast<usize>(length)));
            consumed += headerEnd + static_cast<usize>(length);
        }
    }

    void Encode(const RemoteFrame &frame, List<uint8> &bytes) override
    {
        StringBuilder<96> header("Content-Length: ");
        char digits[24];
        header.Append(StringView(digits, FormatUnsigned(frame.Bytes.Count(), digits))).Append("\r\n");
        if (frame.IsAttachment)
        {
            header.Append("Marlin-Attachment: ")
                .Append(StringView(digits, FormatUnsigned(frame.AttachmentId, digits)))
                .Append("\r\n");
        }
        header.Append("\r\n");
        bytes.Reserve(bytes.Count() + header.Length() + frame.Bytes.Count());
        Append(bytes, header.AsView());
        bytes.AddRange(frame.Bytes.AsSpan());
    }
};

enum class WebSocketOpcode : uint8
{
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

/// RFC 6455 frames, after the upgrade handshake; a client masks what it sends.
class WebSocketFraming final : public IRemoteFraming
{
public:
    explicit WebSocketFraming(bool client) : _client(client)
    {
    }

    bool Decode(Span<const uint8> bytes, usize &consumed, List<RemoteFrame> &frames, List<uint8> &reply) override
    {
        consumed = 0;
        while (bytes.Count() - consumed >= 2)
        {
            const uint8 *head = bytes.Data() + consumed;
            const usize available = bytes.Count() - consumed;
            const bool final = (head[0] & 0x80) != 0;
            const auto opcode = static_cast<WebSocketOpcode>(head[0] & 0x0F);
            const bool masked = (head[1] & 0x80) != 0;
            uint64 size = head[1] & 0x7F;
            usize offset = 2;
            if (size == 126 || size == 127)
            {
                const usize extra = size == 126 ? 2 : 8;
                if (available < offset + extra)
                {
                    return true;
                }
                size = 0;
                for (usize i = 0; i < extra; ++i)
                {
                    size = (size << 8) | head[offset + i];
                }
                offset += extra;
            }
            if (size > RemoteMaxFrameSize || masked == _client)
            {
                return false;
            }
            uint8 key[4] = {};
            if (masked)
            {
                if (available < offset + 4)
                {
                    return true;
                }
                MemCopy(key, head + offset, sizeof(key));
                offset += 4;
            }
            if (available < offset + size)
            {
                return true;
            }
            _payload.ResizeUninitialized(static_cast<usize>(size));
            for (usize i = 0; i < size; ++i)
            {
                _payload[i] = static_cast<uint8>(head[offset + i] ^ (masked ? key[i & 3] : 0));
            }
            consumed += offset + static_cast<usize>(size);
            switch (opcode)
            {
            case WebSocketOpcode::Ping:
                WriteFrame(WebSocketOpcode::Pong, _payload.AsSpan(), reply);
                break;
            case WebSocketOpcode::Pong:
                break;
            case WebSocketOpcode::Close:
                return false;
            case WebSocketOpcode::Text:
            case WebSocketOpcode::Binary:
                _message.Clear();
                _messageIsBinary = opcode == WebSocketOpcode::Binary;
                [[fallthrough]];
            case WebSocketOpcode::Continuation:
                _message.AddRange(_payload.AsSpan());
                if (_message.Count() > RemoteMaxFrameSize || (final && !Complete(frames)))
                {
                    return false;
                }
                break;
            default:
                return false;
            }
        }
        return true;
    }

    void Encode(const RemoteFrame &frame, List<uint8> &bytes) override
    {
        if (!frame.IsAttachment)
        {
            WriteFrame(WebSocketOpcode::Text, frame.Bytes.AsSpan(), bytes);
            return;
        }
        List<uint8> payload;
        payload.ResizeUninitialized(8 + frame.Bytes.Count());
        for (usize i = 0; i < 8; ++i)
        {
            payload[i] = static_cast<uint8>(frame.AttachmentId >> (8 * i));
        }
        MemCopy(payload.Data() + 8, frame.Bytes.Data(), frame.Bytes.Count());
        WriteFrame(WebSocketOpcode::Binary, payload.AsSpan(), bytes);
    }

    void EncodeClose(List<uint8> &bytes) override
    {
        const uint8 code[2] = {0x03, 0xE8};
        WriteFrame(WebSocketOpcode::Close, Span<const uint8>(code, sizeof(code)), bytes);
    }

private:
    void WriteFrame(WebSocketOpcode opcode, Span<const uint8> data, List<uint8> &bytes) const
    {
        const usize size = data.Count();
        uint8 header[14];
        usize length = 0;
        header[length++] = static_cast<uint8>(0x80 | static_cast<uint8>(opcode));
        const uint8 mask = _client ? 0x80 : 0x00;
        if (size < 126)
        {
            header[length++] = static_cast<uint8>(mask | size);
        }
        else if (size <= 0xFFFF)
        {
            header[length++] = static_cast<uint8>(mask | 126);
            header[length++] = static_cast<uint8>(size >> 8);
            header[length++] = static_cast<uint8>(size);
        }
        else
        {
            header[length++] = static_cast<uint8>(mask | 127);
            for (uint32 i = 8; i-- > 0;)
            {
                header[length++] = static_cast<uint8>(static_cast<uint64>(size) >> (8 * i));
            }
        }
        uint8 key[4] = {};
        if (_client)
        {
            PlatformSystem::GetSecureRandomBytes(key, sizeof(key));
            MemCopy(header + length, key, sizeof(key));
            length += sizeof(key);
        }
        const usize start = bytes.Count();
        bytes.ResizeUninitialized(start + length + size);
        MemCopy(bytes.Data() + start, header, length);
        uint8 *payload = bytes.Data() + start + length;
        for (usize i = 0; i < size; ++i)
        {
            payload[i] = static_cast<uint8>(data[i] ^ (_client ? key[i & 3] : 0));
        }
    }

    bool Complete(List<RemoteFrame> &frames)
    {
        RemoteFrame &frame = frames.AddDefaulted();
        if (!_messageIsBinary)
        {
            frame.Bytes = Move(_message);
            _message.Clear();
            return true;
        }
        if (_message.Count() < 8)
        {
            return false;
        }
        frame.IsAttachment = true;
        for (usize i = 0; i < 8; ++i)
        {
            frame.AttachmentId |= static_cast<uint64>(_message[i]) << (8 * i);
        }
        frame.Bytes.AddRange(_message.AsSpan().Slice(8));
        _message.Clear();
        return true;
    }

    List<uint8> _payload;
    List<uint8> _message;
    bool _messageIsBinary = false;
    bool _client;
};

/// Waits for the first bytes of an accepted TCP connection, then becomes a stream or, after answering the upgrade
/// request, a WebSocket.
class AcceptFraming final : public IRemoteFraming
{
public:
    bool Decode(Span<const uint8> bytes, usize &consumed, List<RemoteFrame> &frames, List<uint8> &reply) override
    {
        consumed = 0;
        if (_inner == nullptr)
        {
            if (bytes.Count() < 4)
            {
                return true;
            }
            if (MemCompare(bytes.Data(), "GET ", 4) != 0)
            {
                _inner = RemoteFramings::Stream();
            }
            else
            {
                const usize headEnd = FindHeaderEnd(bytes);
                if (headEnd == 0)
                {
                    return bytes.Count() <= MaxHeaderSize;
                }
                if (!Upgrade(AsText(bytes.Slice(0, headEnd)), reply))
                {
                    return false;
                }
                consumed = headEnd;
                _inner = MakeUnique<WebSocketFraming>(false);
            }
        }
        usize used = 0;
        const bool ok = _inner->Decode(bytes.Slice(consumed), used, frames, reply);
        consumed += used;
        return ok;
    }

    void Encode(const RemoteFrame &frame, List<uint8> &bytes) override
    {
        if (_inner != nullptr)
        {
            _inner->Encode(frame, bytes);
        }
    }

    void EncodeClose(List<uint8> &bytes) override
    {
        if (_inner != nullptr)
        {
            _inner->EncodeClose(bytes);
        }
    }

    [[nodiscard]] bool IsReady() const override
    {
        return _inner != nullptr;
    }

private:
    static bool Upgrade(StringView head, List<uint8> &reply)
    {
        StringView key;
        StringView upgrade;
        if (!FindHeader(head, "Sec-WebSocket-Key", key) || !FindHeader(head, "Upgrade", upgrade) ||
            !upgrade.EqualsIgnoreCase("websocket"))
        {
            Append(reply, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return false;
        }
        StringBuilder<256> response("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: ");
        WebSocketHandshake::AcceptKey(key, response);
        response.Append("\r\n\r\n");
        Append(reply, response.AsView());
        return true;
    }

    UniquePtr<IRemoteFraming> _inner;
};

/// Checks the server's answer to the upgrade request, then frames as a WebSocket client.
class WebSocketClientFraming final : public IRemoteFraming
{
public:
    explicit WebSocketClientFraming(StringView key) : _expected(Expected(key)), _socket(true)
    {
    }

    bool Decode(Span<const uint8> bytes, usize &consumed, List<RemoteFrame> &frames, List<uint8> &reply) override
    {
        consumed = 0;
        if (!_ready)
        {
            const usize headEnd = FindHeaderEnd(bytes);
            if (headEnd == 0)
            {
                return bytes.Count() <= MaxHeaderSize;
            }
            const StringView head = AsText(bytes.Slice(0, headEnd));
            StringView accept;
            if (!head.StartsWith("HTTP/1.1 101") || !FindHeader(head, "Sec-WebSocket-Accept", accept) ||
                accept != _expected.AsView())
            {
                return false;
            }
            consumed = headEnd;
            _ready = true;
        }
        usize used = 0;
        const bool ok = _socket.Decode(bytes.Slice(consumed), used, frames, reply);
        consumed += used;
        return ok;
    }

    void Encode(const RemoteFrame &frame, List<uint8> &bytes) override
    {
        _socket.Encode(frame, bytes);
    }

    void EncodeClose(List<uint8> &bytes) override
    {
        _socket.EncodeClose(bytes);
    }

    [[nodiscard]] bool IsReady() const override
    {
        return _ready;
    }

private:
    static String Expected(StringView key)
    {
        StringBuilder<64> expected;
        WebSocketHandshake::AcceptKey(key, expected);
        return expected.ToString();
    }

    String _expected;
    WebSocketFraming _socket;
    bool _ready = false;
};
} // namespace

namespace RemoteFramings
{
UniquePtr<IRemoteFraming> Stream()
{
    return MakeUnique<StreamFraming>();
}

UniquePtr<IRemoteFraming> AcceptTcp()
{
    return MakeUnique<AcceptFraming>();
}

UniquePtr<IRemoteFraming> WebSocketClient(StringView host, StringView path, List<uint8> &request)
{
    uint8 nonce[16];
    PlatformSystem::GetSecureRandomBytes(nonce, sizeof(nonce));
    StringBuilder<64> key;
    WebSocketHandshake::Base64(nonce, sizeof(nonce), key);
    StringBuilder<512> text;
    text.Append("GET ").Append(path.IsEmpty() ? StringView("/") : path).Append(" HTTP/1.1\r\nHost: ").Append(host);
    text.Append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ");
    text.Append(key.AsView()).Append("\r\n\r\n");
    Append(request, text.AsView());
    return MakeUnique<WebSocketClientFraming>(key.AsView());
}
} // namespace RemoteFramings
} // namespace Marlin

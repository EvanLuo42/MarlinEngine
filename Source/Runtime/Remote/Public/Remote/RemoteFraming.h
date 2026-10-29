#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "String/StringView.h"

namespace Marlin
{
/// JSON-RPC 2.0 error codes; -32000 and below are the application's.
namespace RemoteErrors
{
inline constexpr int32 ParseError = -32700;
inline constexpr int32 InvalidRequest = -32600;
inline constexpr int32 MethodNotFound = -32601;
inline constexpr int32 InvalidParams = -32602;
inline constexpr int32 InternalError = -32603;
inline constexpr int32 Failed = -32000;
inline constexpr int32 NotAuthenticated = -32001;
/// Set by clients when the connection closed or the call timed out.
inline constexpr int32 ConnectionLost = -32002;
} // namespace RemoteErrors

/// One unit on the wire: a JSON-RPC message, or the bytes of an attachment that a message refers to by id.
struct RemoteFrame
{
    List<uint8> Bytes;
    uint64 AttachmentId = 0;
    bool IsAttachment = false;

    [[nodiscard]] StringView AsText() const
    {
        return {reinterpret_cast<const char *>(Bytes.Data()), Bytes.Count()};
    }

    [[nodiscard]] static RemoteFrame FromText(StringView text);
};

/// The largest frame either side accepts.
inline constexpr usize RemoteMaxFrameSize = usize{1} << 30;

/// Turns the bytes of one connection into frames and frames into bytes, with no IO of its own (Documentation/
/// Architecture/14 §1). On a byte stream (named pipe, TCP) each frame is `Content-Length` headers, as in LSP, with a
/// `Marlin-Attachment: <id>` header for attachments; on a WebSocket a message is a text frame and an attachment a
/// binary frame that starts with its 8-byte id. Used by one thread at a time.
class REMOTE_API IRemoteFraming
{
public:
    IRemoteFraming() = default;
    virtual ~IRemoteFraming() = default;
    MARLIN_NONCOPYABLE(IRemoteFraming);

    /// Appends the complete frames in *bytes* and sets *consumed* to the bytes they used; bytes to send back, such
    /// as a handshake answer or a pong, go to *reply*. False if the peer broke the protocol or closed it.
    virtual bool Decode(Span<const uint8> bytes, usize &consumed, List<RemoteFrame> &frames, List<uint8> &reply) = 0;
    virtual void Encode(const RemoteFrame &frame, List<uint8> &bytes) = 0;
    /// The bytes that announce a close, if the protocol has them.
    virtual void EncodeClose(List<uint8> & /*bytes*/)
    {
    }

    /// False until a handshake, if there is one, has completed.
    [[nodiscard]] virtual bool IsReady() const
    {
        return true;
    }
};

namespace RemoteFramings
{
/// `Content-Length` frames, for pipes and plain TCP.
[[nodiscard]] REMOTE_API UniquePtr<IRemoteFraming> Stream();
/// For a connection a TCP listener accepted: a WebSocket upgrade request, recognized by its first 4 bytes, gets
/// the WebSocket framing, anything else the stream framing.
[[nodiscard]] REMOTE_API UniquePtr<IRemoteFraming> AcceptTcp();
/// A WebSocket client; *request* receives the upgrade request to send first, for *host* and request target *path*.
[[nodiscard]] REMOTE_API UniquePtr<IRemoteFraming> WebSocketClient(StringView host, StringView path,
                                                                   List<uint8> &request);
} // namespace RemoteFramings
} // namespace Marlin

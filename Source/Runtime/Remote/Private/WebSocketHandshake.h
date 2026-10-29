#pragma once

#include "CoreTypes.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;

/// The pieces of RFC 6455 the handshake needs.
namespace WebSocketHandshake
{
/// SHA-1 of *data*; only for Sec-WebSocket-Accept, never for security.
REMOTE_API void Sha1(const void *data, usize size, uint8 (&digest)[20]);
REMOTE_API void Base64(const void *data, usize size, StringWriter &output);
/// Sec-WebSocket-Accept for the client's Sec-WebSocket-Key.
REMOTE_API void AcceptKey(StringView key, StringWriter &output);
} // namespace WebSocketHandshake
} // namespace Marlin

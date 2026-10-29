#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "String/StringBuilder.h"
#include "String/StringView.h"

namespace Marlin
{
class JsonValue;
class RemoteServer;

/// Publishes the records of this process to the Log.Stream of *Server* while someone subscribes (10 §5). The records
/// of one Log::Flush go out together at its end, finding the subscribers once.
class REMOTE_API RemoteLogStream final : public ILogSink
{
public:
    static constexpr StringView Stream = "Log.Stream";

    void Write(const LogRecord &record) override;
    void Flush() override;

    /// Writes *record* as one Log.Stream entry: Category, Level, Message, Frame, Thread, Seconds, File, Line, and
    /// Source when given, naming the process a forwarded record came from.
    static void FormatEntry(StringWriter &output, const LogRecord &record, StringView source = {});

    /// Logs a Log.Stream *entry* of another process in this one under *category*, prefixed with its Source, or
    /// *source* when it has none, and its own category. Fatal is logged as Error: it ended the other process.
    static void Relog(const LogCategory &category, const JsonValue &entry, StringView source);

    RemoteServer *Server = nullptr;

private:
    /// The entries of this flush, end to end; Log::Flush runs on one thread at a time.
    StringBuilder<8192> _pending;
    List<usize> _ends;
    /// Whether anyone subscribed when this flush's first record came.
    bool _subscribed = false;
};
} // namespace Marlin

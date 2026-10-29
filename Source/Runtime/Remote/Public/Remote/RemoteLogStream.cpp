#include "Remote/RemoteLogStream.h"

#include "HAL/PlatformTime.h"
#include "Json/JsonValue.h"
#include "Json/JsonWriter.h"
#include "Remote/RemoteServer.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
LogLevel FindLevel(StringView name)
{
    for (uint8 level = 0; level < static_cast<uint8>(LogLevel::Off); ++level)
    {
        if (Log::GetLevelName(static_cast<LogLevel>(level)) == name)
        {
            return static_cast<LogLevel>(level);
        }
    }
    return LogLevel::Info;
}

StringView FindString(const JsonValue &entry, StringView key)
{
    const JsonValue *value = entry.Find(key);
    return value != nullptr ? value->AsString() : StringView();
}
} // namespace

void RemoteLogStream::Write(const LogRecord &record)
{
    if (Server == nullptr)
    {
        return;
    }
    if (_ends.IsEmpty())
    {
        _subscribed = Server->HasSubscribers(Stream);
    }
    if (!_subscribed)
    {
        return;
    }
    FormatEntry(_pending, record);
    _ends.Add(_pending.Length());
}

void RemoteLogStream::Flush()
{
    if (_ends.IsEmpty())
    {
        return;
    }
    if (Server != nullptr)
    {
        List<StringView, InlineAllocator<64>> entries;
        usize start = 0;
        for (const usize end : _ends)
        {
            entries.Add(_pending.AsView().Substring(start, end - start));
            start = end;
        }
        Server->PublishJson(Stream, entries.AsSpan());
    }
    _pending.Clear();
    _ends.Clear();
}

void RemoteLogStream::FormatEntry(StringWriter &output, const LogRecord &record, StringView source)
{
    JsonWriter writer(output);
    writer.BeginObject();
    if (!source.IsEmpty())
    {
        writer.Key("Source").String(source);
    }
    writer.Key("Category").String(record.Category->GetName());
    writer.Key("Level").String(Log::GetLevelName(record.Level)).Key("Message").String(record.Message);
    writer.Key("Frame").Unsigned(record.Frame).Key("Thread").Unsigned(record.ThreadId);
    writer.Key("Seconds").Number(static_cast<float64>(record.Ticks) /
                                 static_cast<float64>(PlatformTime::TicksPerSecond()));
    if (record.File != nullptr)
    {
        writer.Key("File").String(record.File).Key("Line").Integer(record.Line);
    }
    writer.EndObject();
}

void RemoteLogStream::Relog(const LogCategory &category, const JsonValue &entry, StringView source)
{
    LogLevel level = FindLevel(FindString(entry, "Level"));
    level = level == LogLevel::Fatal ? LogLevel::Error : level;
    if (!category.IsEnabled(level))
    {
        return;
    }
    const StringView from = FindString(entry, "Source");
    StringBuilder<512> message;
    message.Append('[').Append(from.IsEmpty() ? source : from).Append("] ");
    message.Append(FindString(entry, "Category")).Append(": ").Append(FindString(entry, "Message"));
    Private::WriteLog(category, level, nullptr, 0, message.AsView());
}
} // namespace Marlin

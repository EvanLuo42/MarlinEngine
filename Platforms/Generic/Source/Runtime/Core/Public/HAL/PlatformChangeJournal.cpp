#include "HAL/PlatformChangeJournal.h"

namespace Marlin
{
ChangeJournal::~ChangeJournal()
{
    Close();
}

bool ChangeJournal::Open(const char * /*directory*/)
{
    return false;
}

void ChangeJournal::Close()
{
    _handle = 0;
}

bool ChangeJournal::GetEnd(JournalCursor & /*cursor*/) const
{
    return false;
}

bool ChangeJournal::Read(JournalCursor & /*cursor*/, void (* /*visitor*/)(const JournalChange &change, void *context),
                         void * /*context*/, uint32 /*maxChanges*/)
{
    return false;
}

usize ChangeJournal::GetPath(uint64 /*fileId*/, char * /*buffer*/, usize /*capacity*/) const
{
    return 0;
}
} // namespace Marlin

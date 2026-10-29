#pragma once

#include "CoreTypes.h"
#include "Remote/RemoteServer.h"
#include "String/String.h"
#include "RemoteTestTypes.gen.h"

namespace Marlin::Tests
{
MSTRUCT()
struct RemoteTestPoint
{
    MGENERATED_BODY()

    MPROPERTY()
    int32 X = 0;
    MPROPERTY()
    int32 Y = 0;
};

MSTRUCT(Category = "Test")
struct RemoteTestMethods
{
    MGENERATED_BODY()

    /// Adds two numbers.
    MFUNCTION(Remote)
    static int32 Add(int32 left, int32 right = 2);

    MFUNCTION(Remote = AnyThread)
    static RemoteTestPoint Mirror(const RemoteTestPoint &point, int32 &sum);

    MFUNCTION(Remote = AnyThread)
    static RemoteAttachment Bytes(uint32 count, RemoteCall &call);

    MFUNCTION(Remote = AnyThread)
    static void Refuse(RemoteCall &call);

    static int32 Calls;
};
} // namespace Marlin::Tests

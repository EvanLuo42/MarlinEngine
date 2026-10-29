#pragma once

#include "Concurrency/ExecutionContext.h"

namespace Marlin::Private
{
extern thread_local ExecutionContext _threadContext;
extern thread_local ExecutionContext *_currentContext;

/// For hot paths inside Core that never switch fibers mid-call; elsewhere use GetExecutionContext.
inline ExecutionContext &CurrentExecutionContext()
{
    ExecutionContext *context = _currentContext;
    return context != nullptr ? *context : _threadContext;
}
} // namespace Marlin::Private

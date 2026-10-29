#include "Concurrency/ExecutionContext.h"

#include "Concurrency/ExecutionContextInternal.h"

namespace Marlin
{
namespace Private
{
thread_local ExecutionContext _threadContext;
thread_local ExecutionContext *_currentContext = nullptr;
} // namespace Private

MARLIN_NOINLINE ExecutionContext &GetExecutionContext()
{
    return Private::CurrentExecutionContext();
}

MARLIN_NOINLINE void SetExecutionContext(ExecutionContext *context)
{
    Private::_currentContext = context;
}
} // namespace Marlin

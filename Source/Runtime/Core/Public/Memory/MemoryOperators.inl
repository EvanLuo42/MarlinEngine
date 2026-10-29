//! Routes the global `operator new` and `operator delete` of one binary to the engine heap.
//!
//! Each executable and DLL compiles this once, from the file `marlin generate` adds to it; nothing else may include
//! it. Every `delete` overload accepts null and requires a pointer from the matching `new`.

#include "Memory/Memory.h"

#include <new>

/// Allocates `size` bytes from the engine heap.
void *operator new(Marlin::usize size)
{
    return Marlin::Memory::Allocate(size);
}

/// Allocates `size` bytes for an array from the engine heap.
void *operator new[](Marlin::usize size)
{
    return Marlin::Memory::Allocate(size);
}

/// Allocates `size` bytes; failure is fatal, so this never returns null.
void *operator new(Marlin::usize size, const std::nothrow_t & /*tag*/) noexcept
{
    return Marlin::Memory::Allocate(size);
}

/// Allocates `size` bytes for an array; failure is fatal, so this never returns null.
void *operator new[](Marlin::usize size, const std::nothrow_t & /*tag*/) noexcept
{
    return Marlin::Memory::Allocate(size);
}

/// Allocates `size` bytes aligned to `alignment` from the engine heap.
void *operator new(Marlin::usize size, std::align_val_t alignment)
{
    return Marlin::Memory::Allocate(size, static_cast<Marlin::usize>(alignment));
}

/// Allocates `size` bytes for an array, aligned to `alignment`.
void *operator new[](Marlin::usize size, std::align_val_t alignment)
{
    return Marlin::Memory::Allocate(size, static_cast<Marlin::usize>(alignment));
}

/// Allocates `size` bytes aligned to `alignment`; failure is fatal, so this never returns null.
void *operator new(Marlin::usize size, std::align_val_t alignment, const std::nothrow_t & /*tag*/) noexcept
{
    return Marlin::Memory::Allocate(size, static_cast<Marlin::usize>(alignment));
}

/// Allocates `size` bytes for an array, aligned to `alignment`; failure is fatal, so this never returns null.
void *operator new[](Marlin::usize size, std::align_val_t alignment, const std::nothrow_t & /*tag*/) noexcept
{
    return Marlin::Memory::Allocate(size, static_cast<Marlin::usize>(alignment));
}

/// Releases memory from the matching `operator new`.
void operator delete(void *pointer) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching array `operator new`.
void operator delete[](void *pointer) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching sized `operator new`; `size` is ignored.
void operator delete(void *pointer, Marlin::usize /*size*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching sized array `operator new`; `size` is ignored.
void operator delete[](void *pointer, Marlin::usize /*size*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching nothrow `operator new`.
void operator delete(void *pointer, const std::nothrow_t & /*tag*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching nothrow array `operator new`.
void operator delete[](void *pointer, const std::nothrow_t & /*tag*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching aligned `operator new`; `alignment` is ignored.
void operator delete(void *pointer, std::align_val_t /*alignment*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching aligned array `operator new`; `alignment` is ignored.
void operator delete[](void *pointer, std::align_val_t /*alignment*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching sized, aligned `operator new`.
void operator delete(void *pointer, Marlin::usize /*size*/, std::align_val_t /*alignment*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching sized, aligned array `operator new`.
void operator delete[](void *pointer, Marlin::usize /*size*/, std::align_val_t /*alignment*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching aligned nothrow `operator new`.
void operator delete(void *pointer, std::align_val_t /*alignment*/, const std::nothrow_t & /*tag*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

/// Releases memory from the matching aligned nothrow array `operator new`.
void operator delete[](void *pointer, std::align_val_t /*alignment*/, const std::nothrow_t & /*tag*/) noexcept
{
    Marlin::Memory::Free(pointer);
}

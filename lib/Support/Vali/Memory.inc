//===- Vali/Memory.cpp - Vali Memory Implementation -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the Vali specific implementation of various Memory
// management utilities
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/DataTypes.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Process.h"
#include <os/mollenos.h>

namespace {

Flags_t getValiProtectionFlags(unsigned Flags) {
    Flags_t Result = 0;
    if (Flags & llvm::sys::Memory::MF_READ) {
        Result |= MEMORY_READ;
    }
    if (Flags & llvm::sys::Memory::MF_WRITE) {
        Result |= MEMORY_WRITE;
    }
    if (Flags & llvm::sys::Memory::MF_EXEC) {
        Result |= MEMORY_EXECUTABLE;
    }
    return Result;
}

size_t getPageSize() {
    SystemDescriptor_t Descriptor;
    if (SystemQuery(&Descriptor) != OsSuccess) {
        // Default to 0x1000
        return 0x1000;
    }
    return Descriptor.PageSizeBytes;
}

} // namespace

namespace llvm {
namespace sys {

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only Win32 specific code
//===          and must not be UNIX code
//===----------------------------------------------------------------------===//

MemoryBlock Memory::allocateMappedMemory(size_t NumBytes,
                                         const MemoryBlock *const NearBlock,
                                         unsigned Flags,
                                         std::error_code &EC) {
  EC = std::error_code();
  if (NumBytes == 0)
    return MemoryBlock();

  // While we'd be happy to allocate single pages, the Windows allocation
  // granularity may be larger than a single page (in practice, it is 64K)
  // so mapping less than that will create an unreachable fragment of memory.
  // Avoid using one-time initialization of static locals here, since they
  // aren't thread safe with MSVC.
  static volatile size_t GranularityCached;
  size_t Granularity = GranularityCached;
  if (Granularity == 0) {
    Granularity = getPageSize();
    GranularityCached = Granularity;
  }

  const size_t NumBlocks = (NumBytes+Granularity-1)/Granularity;

  uintptr_t Start = NearBlock ? reinterpret_cast<uintptr_t>(NearBlock->base()) +
                                NearBlock->size()
                           : 0;

  // If the requested address is not aligned to the allocation granularity,
  // round up to get beyond NearBlock. VirtualAlloc would have rounded down.
  if (Start && Start % Granularity != 0)
    Start += Granularity - Start % Granularity;

  Flags_t Protect = getValiProtectionFlags(Flags);
  void *PA = NULL;
  if (::MemoryAllocate(reinterpret_cast<void*>(Start),
                            NumBlocks*Granularity, MEMORY_COMMIT | Protect, 
                            &PA, NULL) != OsSuccess) {
    return MemoryBlock();
  }

  MemoryBlock Result;
  Result.Address = PA;
  Result.Size = NumBlocks*Granularity;

  if (Flags & MF_EXEC)
    Memory::InvalidateInstructionCache(Result.Address, Result.Size);

  return Result;
}

  std::error_code Memory::releaseMappedMemory(MemoryBlock &M) {
  if (M.Address == 0 || M.Size == 0)
    return std::error_code();

  if (MemoryFree(M.Address, M.Size) != OsSuccess)
    return std::error_code();

  M.Address = 0;
  M.Size = 0;

  return std::error_code();
}

  std::error_code Memory::protectMappedMemory(const MemoryBlock &M,
                                       unsigned Flags) {
  if (M.Address == 0 || M.Size == 0)
    return std::error_code();

  Flags_t Protect = getValiProtectionFlags(Flags);
  Flags_t Previous = 0;
  if (MemoryProtect(M.Address, M.Size, Protect, &Previous) != OsSuccess)
    return std::error_code();

  if (Flags & MF_EXEC)
    Memory::InvalidateInstructionCache(M.Address, M.Size);

  return std::error_code();
}

/// InvalidateInstructionCache - Before the JIT can run a block of code
/// that has been emitted it must invalidate the instruction cache on some
/// platforms.
void Memory::InvalidateInstructionCache(const void *Addr, size_t Len) {
  FlushHardwareCache(CACHE_INSTRUCTION, const_cast<void*>(Addr), Len);
}

} // namespace sys
} // namespace llvm

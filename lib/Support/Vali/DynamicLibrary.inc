//===- Vali/DynamicLibrary.cpp - Vali DL Implementation -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the Vali specific implementation of DynamicLibrary.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/raw_ostream.h"
#include <os/sharedobject.h>

DynamicLibrary::HandleSet::~HandleSet() {
  for (void *Handle : llvm::reverse(Handles))
    SharedObjectUnload(Handle_t(Handle));

  // 'Process' should not be released on Windows.
  assert((!Process || Process==this) && "Bad Handle");
  // llvm_shutdown called, Return to default
  DynamicLibrary::SearchOrder = DynamicLibrary::SO_Linker;
}

void *DynamicLibrary::HandleSet::DLOpen(const char *File, std::string *Err) {
  // Create the instance and return it to be the *Process* handle
  // simillar to dlopen(NULL, RTLD_LAZY|RTLD_GLOBAL)
  if (!File)
    return &(*OpenedHandles);

  Handle_t Handle = SharedObjectLoad(FileUnicode.data());
  if (Handle == NULL) {
    MakeErrMsg(Err, std::string(File) + ": Can't open");
    return &DynamicLibrary::Invalid;
  }

  return reinterpret_cast<void*>(Handle);
}

static DynamicLibrary::HandleSet *IsOpenedHandlesInstance(void *Handle) {
  if (!OpenedHandles.isConstructed())
    return nullptr;
  DynamicLibrary::HandleSet &Inst = *OpenedHandles;
  return Handle == &Inst ? &Inst : nullptr;
}

void DynamicLibrary::HandleSet::DLClose(void *Handle) {
  if (HandleSet* HS = IsOpenedHandlesInstance(Handle))
    HS->Process = nullptr; // Just drop the *Process* handle.
  else
    FreeLibrary((HMODULE)Handle);
}

static bool GetProcessModules(HANDLE H, DWORD &Bytes, HMODULE *Data = nullptr) {
  // EnumProcessModules will fail on Windows 64 while some versions of
  // MingW-32 don't have EnumProcessModulesEx.
  if (
#ifdef _WIN64
      !EnumProcessModulesEx(H, Data, Bytes, &Bytes, LIST_MODULES_64BIT)
#else
      !EnumProcessModules(H, Data, Bytes, &Bytes)
#endif
     ) {
    std::string Err;
    if (MakeErrMsg(&Err, "EnumProcessModules failure"))
      llvm::errs() << Err << "\n";
    return false;
  }
  return true;
}

void *DynamicLibrary::HandleSet::DLSym(void *Handle, const char *Symbol) {
  HandleSet* HS = IsOpenedHandlesInstance(Handle);
  if (!HS)
    return SharedObjectGetFunction((Handle_t)Handle, Symbol);

  // Could have done a dlclose on the *Process* handle
  if (!HS->Process)
    return nullptr;

  DWORD Bytes = 0;
  HMODULE Self = HMODULE(GetCurrentProcess());
  if (!GetProcessModules(Self, Bytes))
    return nullptr;

  // Get the most recent list in case any modules added/removed between calls
  // to EnumProcessModulesEx that gets the amount of, then copies the HMODULES.
  // MSDN is pretty clear that if the module list changes during the call to
  // EnumProcessModulesEx the results should not be used.
  std::vector<HMODULE> Handles;
  do {
    assert(Bytes && ((Bytes % sizeof(HMODULE)) == 0) &&
           "Should have at least one module and be aligned");
    Handles.resize(Bytes / sizeof(HMODULE));
    if (!GetProcessModules(Self, Bytes, Handles.data()))
      return nullptr;
  } while (Bytes != (Handles.size() * sizeof(HMODULE)));

  // Try EXE first, mirroring what dlsym(dlopen(NULL)) does.
  if (FARPROC Ptr = GetProcAddress(HMODULE(Handles.front()), Symbol))
    return (void *) uintptr_t(Ptr);

  if (Handles.size() > 1) {
    // This is different behaviour than what Posix dlsym(dlopen(NULL)) does.
    // Doing that here is causing real problems for the JIT where msvc.dll
    // and ucrt.dll can define the same symbols. The runtime linker will choose
    // symbols from ucrt.dll first, but iterating NOT in reverse here would
    // mean that the msvc.dll versions would be returned.

    for (auto I = Handles.rbegin(), E = Handles.rend()-1; I != E; ++I) {
      if (FARPROC Ptr = GetProcAddress(HMODULE(*I), Symbol))
        return (void *) uintptr_t(Ptr);
    }
  }
  return nullptr;
}


// Stack probing routines are in the support library (e.g. libgcc), but we don't
// have dynamic linking on windows. Provide a hook.
#define EXPLICIT_SYMBOL(SYM)                    \
  extern "C" { extern void *SYM; }
#define EXPLICIT_SYMBOL2(SYMFROM, SYMTO) EXPLICIT_SYMBOL(SYMTO)

#ifdef _M_IX86
// Win32 on x86 implements certain single-precision math functions as macros.
// These functions are not exported by the DLL, but will still be needed
// for symbol-resolution by the JIT loader. Therefore, this Support libray
// provides helper functions with the same implementation.

#define INLINE_DEF_SYMBOL1(TYP, SYM)                                           \
  extern "C" TYP inline_##SYM(TYP _X) { return SYM(_X); }
#define INLINE_DEF_SYMBOL2(TYP, SYM)                                           \
  extern "C" TYP inline_##SYM(TYP _X, TYP _Y) { return SYM(_X, _Y); }
#endif

#include "explicit_symbols.inc"

#undef EXPLICIT_SYMBOL
#undef EXPLICIT_SYMBOL2
#undef INLINE_DEF_SYMBOL1
#undef INLINE_DEF_SYMBOL2

static void *DoSearch(const char *SymbolName) {

#define EXPLICIT_SYMBOL(SYM)                                                   \
  if (!strcmp(SymbolName, #SYM))                                               \
    return (void *)&SYM;
#define EXPLICIT_SYMBOL2(SYMFROM, SYMTO)                                       \
  if (!strcmp(SymbolName, #SYMFROM))                                           \
    return (void *)&SYMTO;

#ifdef _M_IX86
#define INLINE_DEF_SYMBOL1(TYP, SYM)                                           \
  if (!strcmp(SymbolName, #SYM))                                               \
    return (void *)&inline_##SYM;
#define INLINE_DEF_SYMBOL2(TYP, SYM) INLINE_DEF_SYMBOL1(TYP, SYM)
#endif

  {
#include "explicit_symbols.inc"
  }

#undef EXPLICIT_SYMBOL
#undef EXPLICIT_SYMBOL2
#undef INLINE_DEF_SYMBOL1
#undef INLINE_DEF_SYMBOL2

  return nullptr;
}

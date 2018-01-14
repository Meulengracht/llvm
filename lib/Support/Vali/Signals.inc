//===- Signals.cpp - Generic Unix Signals Implementation -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines some helpful functions for dealing with the possibility of
// Unix signals occurring while your program is running.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/UniqueLock.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <string>
#include <unwind.h>
#include <io.h>
#if HAVE_SIGNAL_H
#include <signal.h>
#endif

using namespace llvm;

static RETSIGTYPE SignalHandler(int Sig);  // defined below.

static ManagedStatic<sys::SmartMutex<true> > SignalsMutex;

/// InterruptFunction - The function to call if ctrl-c is pressed.
static void (*InterruptFunction)() = nullptr;

static ManagedStatic<std::vector<std::string>> FilesToRemove;

static StringRef Argv0;

// IntSigs - Signals that represent requested termination. There's no bug
// or failure, or if there is, it's not our direct responsibility. For whatever
// reason, our continued execution is no longer desirable.
static const int IntSigs[] = {
  SIGHUP, SIGINT, SIGPIPE, SIGTERM, SIGUSR1, SIGUSR2
};

// KillSigs - Signals that represent that we have a bug, and our prompt
// termination has been ordered.
static const int KillSigs[] = {
  SIGILL, SIGTRAP, SIGABRT, SIGFPE, SIGBUS, SIGSEGV, SIGQUIT
#ifdef SIGSYS
  , SIGSYS
#endif
#ifdef SIGXCPU
  , SIGXCPU
#endif
#ifdef SIGXFSZ
  , SIGXFSZ
#endif
#ifdef SIGEMT
  , SIGEMT
#endif
};

static unsigned NumRegisteredSignals = 0;
static struct {
  __signalhandler_t SA;
  int SigNo;
} RegisteredSignalInfo[array_lengthof(IntSigs) + array_lengthof(KillSigs)];


static void RegisterHandler(int Signal) {
  assert(NumRegisteredSignals < array_lengthof(RegisteredSignalInfo) &&
         "Out of space for signal handlers!");
         
  // Install the new handler, save the old one in RegisteredSignalInfo.
  RegisteredSignalInfo[NumRegisteredSignals].SA = signal(Signal, SignalHandler);
  RegisteredSignalInfo[NumRegisteredSignals].SigNo = Signal;
  ++NumRegisteredSignals;
}

static void RegisterHandlers() {
  sys::SmartScopedLock<true> Guard(*SignalsMutex);

  // If the handlers are already registered, we're done.
  if (NumRegisteredSignals != 0) return;
  for (auto S : IntSigs) RegisterHandler(S);
  for (auto S : KillSigs) RegisterHandler(S);
}

static void UnregisterHandlers() {
  // Restore all of the signal handlers to how they were before we showed up.
  for (unsigned i = 0, e = NumRegisteredSignals; i != e; ++i)
    signal(RegisteredSignalInfo[i].SigNo, RegisteredSignalInfo[i].SA);
  NumRegisteredSignals = 0;
}


/// RemoveFilesToRemove - Process the FilesToRemove list. This function
/// should be called with the SignalsMutex lock held.
/// NB: This must be an async signal safe function. It cannot allocate or free
/// memory, even in debug builds.
static void RemoveFilesToRemove() {
  // Avoid constructing ManagedStatic in the signal handler.
  // If FilesToRemove is not constructed, there are no files to remove.
  if (!FilesToRemove.isConstructed())
    return;

  // We avoid iterators in case of debug iterators that allocate or release
  // memory.
  std::vector<std::string>& FilesToRemoveRef = *FilesToRemove;
  for (unsigned i = 0, e = FilesToRemoveRef.size(); i != e; ++i) {
    const char *path = FilesToRemoveRef[i].c_str();

    // Otherwise, remove the file. We ignore any errors here as there is nothing
    // else we can do.
    _unlink(path);
  }
}

// SignalHandler - The signal handler that runs.
static RETSIGTYPE SignalHandler(int Sig) {
  // Restore the signal behavior to default, so that the program actually
  // crashes when we return and the signal reissues.  This also ensures that if
  // we crash in our signal handler that the program will terminate immediately
  // instead of recursing in the signal handler.
  UnregisterHandlers();

  {
    unique_lock<sys::SmartMutex<true>> Guard(*SignalsMutex);
    RemoveFilesToRemove();

    if (std::find(std::begin(IntSigs), std::end(IntSigs), Sig)
        != std::end(IntSigs)) {
      if (InterruptFunction) {
        void (*IF)() = InterruptFunction;
        Guard.unlock();
        InterruptFunction = nullptr;
        IF();        // run the interrupt function.
        return;
      }

      Guard.unlock();
      raise(Sig);   // Execute the default handler.
      return;
   }
  }

  // Otherwise if it is a fault (like SEGV) run any handler.
  llvm::sys::RunSignalHandlers();
}

void llvm::sys::RunInterruptHandlers() {
  sys::SmartScopedLock<true> Guard(*SignalsMutex);
  RemoveFilesToRemove();
}

void llvm::sys::SetInterruptFunction(void (*IF)()) {
  {
    sys::SmartScopedLock<true> Guard(*SignalsMutex);
    InterruptFunction = IF;
  }
  RegisterHandlers();
}

// RemoveFileOnSignal - The public API
bool llvm::sys::RemoveFileOnSignal(StringRef Filename,
                                   std::string* ErrMsg) {
  {
    sys::SmartScopedLock<true> Guard(*SignalsMutex);
    FilesToRemove->push_back(Filename);
  }

  RegisterHandlers();
  return false;
}

// DontRemoveFileOnSignal - The public API
void llvm::sys::DontRemoveFileOnSignal(StringRef Filename) {
  sys::SmartScopedLock<true> Guard(*SignalsMutex);
  std::vector<std::string>::reverse_iterator RI =
      find(reverse(*FilesToRemove), Filename);
  std::vector<std::string>::iterator I = FilesToRemove->end();
  if (RI != FilesToRemove->rend())
    I = FilesToRemove->erase(RI.base()-1);
}

/// AddSignalHandler - Add a function to be called when a signal is delivered
/// to the process.  The handler can have a cookie passed to it to identify
/// what instance of the handler it is.
void llvm::sys::AddSignalHandler(void (*FnPtr)(void *), void *Cookie) {
  CallBacksToRun->push_back(std::make_pair(FnPtr, Cookie));
  RegisterHandlers();
}


/// This platform does not have dl_iterate_phdr, so we do not yet know how to
/// find all loaded DSOs.
static bool findModulesAndOffsets(void **StackTrace, int Depth,
                                  const char **Modules, intptr_t *Offsets,
                                  const char *MainExecutableName,
                                  StringSaver &StrPool) {
  return false;
}

static int unwindBacktrace(void **StackTrace, int MaxEntries) {
  if (MaxEntries < 0)
    return 0;

  // Skip the first frame ('unwindBacktrace' itself).
  int Entries = -1;

  auto HandleFrame = [&](_Unwind_Context *Context) -> _Unwind_Reason_Code {
    // Apparently we need to detect reaching the end of the stack ourselves.
    void *IP = (void *)_Unwind_GetIP(Context);
    if (!IP)
      return _URC_END_OF_STACK;

    assert(Entries < MaxEntries && "recursively called after END_OF_STACK?");
    if (Entries >= 0)
      StackTrace[Entries] = IP;

    if (++Entries == MaxEntries)
      return _URC_END_OF_STACK;
    return _URC_NO_REASON;
  };

  _Unwind_Backtrace(
      [](_Unwind_Context *Context, void *Handler) {
        return (*static_cast<decltype(HandleFrame) *>(Handler))(Context);
      },
      static_cast<void *>(&HandleFrame));
  return std::max(Entries, 0);
}

// PrintStackTrace - In the case of a program crash or fault, print out a stack
// trace so that the user has an indication of why and where we died.
//
// On glibc systems we have the 'backtrace' function, which works nicely, but
// doesn't demangle symbols.
void llvm::sys::PrintStackTrace(raw_ostream &OS) {
#if ENABLE_BACKTRACES
  static void *StackTrace[256];
  int depth = 0;
#if defined(HAVE_BACKTRACE)
  // Use backtrace() to output a backtrace on Linux systems with glibc.
  if (!depth)
    depth = backtrace(StackTrace, static_cast<int>(array_lengthof(StackTrace)));
#endif
#if defined(HAVE__UNWIND_BACKTRACE)
  // Try _Unwind_Backtrace() if backtrace() failed.
  if (!depth)
    depth = unwindBacktrace(StackTrace,
                        static_cast<int>(array_lengthof(StackTrace)));
#endif
  if (!depth)
    return;

  if (printSymbolizedStackTrace(Argv0, StackTrace, depth, OS))
    return;
#if HAVE_DLFCN_H && HAVE_DLADDR
  int width = 0;
  for (int i = 0; i < depth; ++i) {
    Dl_info dlinfo;
    dladdr(StackTrace[i], &dlinfo);
    const char* name = strrchr(dlinfo.dli_fname, '/');

    int nwidth;
    if (!name) nwidth = strlen(dlinfo.dli_fname);
    else       nwidth = strlen(name) - 1;

    if (nwidth > width) width = nwidth;
  }

  for (int i = 0; i < depth; ++i) {
    Dl_info dlinfo;
    dladdr(StackTrace[i], &dlinfo);

    OS << format("%-2d", i);

    const char* name = strrchr(dlinfo.dli_fname, '/');
    if (!name) OS << format(" %-*s", width, dlinfo.dli_fname);
    else       OS << format(" %-*s", width, name+1);

    OS << format(" %#0*lx", (int)(sizeof(void*) * 2) + 2,
                 (unsigned long)StackTrace[i]);

    if (dlinfo.dli_sname != nullptr) {
      OS << ' ';
      int res;
      char* d = itaniumDemangle(dlinfo.dli_sname, nullptr, nullptr, &res);
      if (!d) OS << dlinfo.dli_sname;
      else    OS << d;
      free(d);

      // FIXME: When we move to C++11, use %t length modifier. It's not in
      // C++03 and causes gcc to issue warnings. Losing the upper 32 bits of
      // the stack offset for a stack dump isn't likely to cause any problems.
      OS << format(" + %u",(unsigned)((char*)StackTrace[i]-
                                      (char*)dlinfo.dli_saddr));
    }
    OS << '\n';
  }
#elif defined(HAVE_BACKTRACE)
  backtrace_symbols_fd(StackTrace, depth, STDERR_FILENO);
#endif
#endif
}

static void PrintStackTraceSignalHandler(void *) {
  sys::PrintStackTrace(llvm::errs());
}

void llvm::sys::DisableSystemDialogsOnCrash() {}

/// PrintStackTraceOnErrorSignal - When an error signal (such as SIGABRT or
/// SIGSEGV) is delivered to the process, print a stack trace and then exit.
void llvm::sys::PrintStackTraceOnErrorSignal(StringRef Argv0,
                                             bool DisableCrashReporting) {
  ::Argv0 = Argv0;

  AddSignalHandler(PrintStackTraceSignalHandler, nullptr);
}

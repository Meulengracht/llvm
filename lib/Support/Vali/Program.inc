//===- Vali/Program.cpp - Vali Program Implementation ------- -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the Vali specific implementation of the Program class.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <io.h>
#include <malloc.h>
#include <os/mollenos.h>
#include <os/process.h>

namespace llvm {

ProcessInfo::ProcessInfo() : ProcessHandle(0), Pid(0), ReturnCode(0) {}

ErrorOr<std::string> sys::findProgramByName(StringRef Name,
                                            ArrayRef<StringRef> Paths) {
  assert(!Name.empty() && "Must have a name!");
  // Use the given path verbatim if it contains any slashes; this matches
  // the behavior of sh(1) and friends.
  if (Name.find_first_of("/\\") != StringRef::npos)
    return std::string(Name);

  SmallVector<StringRef, 12> PathExts;
  PathExts.push_back("");
  PathExts.push_back(".app");

  SmallVector<StringRef, 16> EnvironmentPaths;
  if (Paths.empty())
    if (const char *PathEnv = std::getenv("PATH")) {
      SplitString(PathEnv, EnvironmentPaths, ":");
      Paths = EnvironmentPaths;
    }
  
  for (StringRef Ext : PathExts) {
    for (auto Path : Paths) {
        if (Path.empty())
            continue;

        // Check to see if this first directory contains the executable...
        SmallString<128> FilePath(Path);
        sys::path::append(FilePath, Name);
        sys::path::append(FilePath, Ext);
        if (sys::fs::can_execute(FilePath.c_str()))
            return std::string(FilePath.str()); // Found the executable!
    }
  }
  
  return errc::no_such_file_or_directory;
}

/// ArgNeedsQuotes - Check whether argument needs to be quoted when calling
/// CreateProcess.
static bool ArgNeedsQuotes(const char *Str) {
  return Str[0] == '\0' || strpbrk(Str, "\t \"&\'()*<>\\`^|") != 0;
}

/// CountPrecedingBackslashes - Returns the number of backslashes preceding Cur
/// in the C string Start.
static unsigned int CountPrecedingBackslashes(const char *Start,
                                              const char *Cur) {
  unsigned int Count = 0;
  --Cur;
  while (Cur >= Start && *Cur == '\\') {
    ++Count;
    --Cur;
  }
  return Count;
}

/// EscapePrecedingEscapes - Append a backslash to Dst for every backslash
/// preceding Cur in the Start string.  Assumes Dst has enough space.
static char *EscapePrecedingEscapes(char *Dst, const char *Start,
                                    const char *Cur) {
  unsigned PrecedingEscapes = CountPrecedingBackslashes(Start, Cur);
  while (PrecedingEscapes > 0) {
    *Dst++ = '\\';
    --PrecedingEscapes;
  }
  return Dst;
}

/// ArgLenWithQuotes - Check whether argument needs to be quoted when calling
/// CreateProcess and returns length of quoted arg with escaped quotes
static unsigned int ArgLenWithQuotes(const char *Str) {
  const char *Start = Str;
  bool Quoted = ArgNeedsQuotes(Str);
  unsigned int len = Quoted ? 2 : 0;

  while (*Str != '\0') {
    if (*Str == '\"') {
      // We need to add a backslash, but ensure that it isn't escaped.
      unsigned PrecedingEscapes = CountPrecedingBackslashes(Start, Str);
      len += PrecedingEscapes + 1;
    }
    // Note that we *don't* need to escape runs of backslashes that don't
    // precede a double quote!  See MSDN:
    // http://msdn.microsoft.com/en-us/library/17w5ykft%28v=vs.85%29.aspx

    ++len;
    ++Str;
  }

  if (Quoted) {
    // Make sure the closing quote doesn't get escaped by a trailing backslash.
    unsigned PrecedingEscapes = CountPrecedingBackslashes(Start, Str);
    len += PrecedingEscapes + 1;
  }

  return len;
}

}

static std::unique_ptr<char[]> flattenArgs(const char **Args) {
  // First, determine the length of the command line.
  unsigned len = 0;
  for (unsigned i = 0; Args[i]; i++) {
    len += ArgLenWithQuotes(Args[i]) + 1;
  }

  // Now build the command line.
  std::unique_ptr<char[]> command(new char[len+1]);
  char *p = command.get();

  for (unsigned i = 0; Args[i]; i++) {
    const char *arg = Args[i];
    const char *start = arg;

    bool needsQuoting = ArgNeedsQuotes(arg);
    if (needsQuoting)
      *p++ = '"';

    while (*arg != '\0') {
      if (*arg == '\"') {
        // Escape all preceding escapes (if any), and then escape the quote.
        p = EscapePrecedingEscapes(p, start, arg);
        *p++ = '\\';
      }

      *p++ = *arg++;
    }

    if (needsQuoting) {
      // Make sure our quote doesn't get escaped by a trailing backslash.
      p = EscapePrecedingEscapes(p, start, arg);
      *p++ = '"';
    }
    *p++ = ' ';
  }

  *p = 0;
  return command;
}

// Implemented
static bool Execute(ProcessInfo &PI, StringRef Program, const char **Args,
                    const char **Envp, ArrayRef<Optional<StringRef>> Redirects,
                    unsigned MemoryLimit, std::string *ErrMsg) {
  if (!sys::fs::can_execute(Program)) {
    if (ErrMsg)
      *ErrMsg = "program not executable";
    return false;
  }

  // can_execute may succeed by looking at Program + ".app". 
  SmallString<64> ProgramStorage;
  if (!sys::fs::exists(Program))
    Program = Twine(Program + ".app").toStringRef(ProgramStorage);

  // Vali wants a command line, not an array of args, to pass to the new
  // process.  We have to concatenate them all, while quoting the args that
  // have embedded spaces (or are empty).
  std::unique_ptr<char[]> command = flattenArgs(Args);
  PI.ProcessId = ProcessSpawn(Program.begin(), command.get(), 0);
  if (PI.ProcessId == UUID_INVALID) {
      return false;
  }
  return true;
}

// Implemented
namespace llvm {
ProcessInfo sys::Wait(const ProcessInfo &PI, unsigned SecondsToWait,
                      bool WaitUntilChildTerminates, std::string *ErrMsg) {
  assert(PI.Pid && "invalid pid to wait on, process not started?");
  assert(PI.ProcessId != UUID_INVALID &&
         "invalid process handle to wait on, process not started?");
  size_t milliSecondsToWait = 0;
  ProcessInfo WaitResult = PI;
  if (!WaitUntilChildTerminates && SecondsToWait > 0)
    milliSecondsToWait = SecondsToWait * 1000;

  // Wait for process
  if (ProcessJoin(PI.ProcessId, milliSecondsToWait, &WaitResult.ReturnCode) != OsSuccess) {
      // Timeout occurred
      if (SecondsToWait) {
          // Kill it
          if (ProcessKill(PI.ProcessId) != OsSuccess) {
              ErrMsg = std::string("Failed to kill the process after wait timeout.");
          }
          return WaitResult;
      }
      else {
          return ProcessInfo();
      }
  }
  return WaitResult;
}

// Implemented
  std::error_code sys::ChangeStdinToBinary(){
  // Do nothing, as Vali doesn't differentiate between text and binary.
    return std::error_code();
}

// Implemented
  std::error_code sys::ChangeStdoutToBinary(){
  // Do nothing, as Vali doesn't differentiate between text and binary.
    return std::error_code();
}

// Implemented
std::error_code
llvm::sys::writeFileWithEncoding(StringRef FileName, StringRef Contents,
                                 WindowsEncodingMethod Encoding /*unused*/) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(FileName, EC, llvm::sys::fs::OpenFlags::F_Text);

  if (EC)
    return EC;

  OS << Contents;

  if (OS.has_error())
    return make_error_code(errc::io_error);

  return EC;
}

// Implemented
bool llvm::sys::commandLineFitsWithinSystemLimits(StringRef Program,
                                                  ArrayRef<const char *> Args) {
  // The documented max length of the command line passed to CreateProcess.
  static const size_t MaxCommandStringLength = 32768;
  // Account for the trailing space for the program path and the
  // trailing NULL of the last argument.
  size_t ArgLength = ArgLenWithQuotes(Program.str().c_str()) + 2;
  for (const char* Arg : Args) {
    // Account for the trailing space for every arg
    ArgLength += ArgLenWithQuotes(Arg) + 1;
    if (ArgLength > MaxCommandStringLength) {
      return false;
    }
  }
  return true;
}
}

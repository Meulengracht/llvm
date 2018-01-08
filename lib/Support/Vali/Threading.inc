//===- Vali/Threading.inc - Vali Threading Implementation ----- -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the Vali specific implementation of Threading functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include <os/mollenos.h>
#include <threads.h>

namespace {
  struct ThreadInfo {
    void(*UserFn)(void *);
    void *UserData;
  };
}

static int ExecuteOnThread_Dispatch(void *Arg) {
  ThreadInfo *TI = reinterpret_cast<ThreadInfo*>(Arg);
  TI->UserFn(TI->UserData);
  return 0;
}

void llvm::llvm_execute_on_thread(void(*Fn)(void*), void *UserData,
  unsigned RequestedStackSize) {
  ThreadInfo Info = { Fn, UserData };
  thrd_t Thread;
  int res;

  // Construct and execute the thread.
  if (::thrd_create(&Thread, ExecuteOnThread_Dispatch, &Info) != thrd_success) return;
  ::thrd_join(Thread, &res); // Wait for the thread and clean up.
}


uint64_t llvm::get_threadid() {
  return uint64_t(thrd_current());
}


static constexpr uint32_t get_max_thread_name_length_impl() {
    return 64;
}

uint32_t llvm::get_max_thread_name_length() {
  return get_max_thread_name_length_impl();
}

void llvm::set_thread_name(const Twine &Name) {
  // Make sure the input is null terminated.
  SmallString<64> Storage;
  StringRef NameStr = Name.toNullTerminatedStringRef(Storage);

  // Truncate from the beginning, not the end, if the specified name is too
  // long.  For one, this ensures that the resulting string is still null
  // terminated, but additionally the end of a long thread name will usually
  // be more unique than the beginning, since a common pattern is for similar
  // threads to share a common prefix.
  if (get_max_thread_name_length() > 0)
    NameStr = NameStr.take_back(get_max_thread_name_length());
  (void)NameStr;
  ::SetCurrentThreadName(NameStr.data());
}

void llvm::get_thread_name(SmallVectorImpl<char> &Name) {
  Name.clear();
  char Buffer[get_max_thread_name_length_impl()] = { 0 };
  GetCurrentThreadName(&Buffer[0], get_max_thread_name_length_impl());
  Name.append(Buffer, Buffer + strlen(Buffer));
}

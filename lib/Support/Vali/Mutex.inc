//===- llvm/Support/Vali/Mutex.inc - Vali Mutex Implementation -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Vali specific (non-pthread) Mutex class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Mutex.h"
#include <threads.h>
#include <stdlib.h>

namespace llvm {

sys::MutexImpl::MutexImpl(bool recursive)
{
  mtx_t* mtx = (mtx_t*)malloc(sizeof(mtx_t));
  if (recursive) mtx_init(mtx, mtx_recursive);
  else mtx_init(mtx, mtx_plain);
  data_ = mtx;
}

sys::MutexImpl::~MutexImpl()
{
  if (data_) {
    mtx_destroy((mtx_t*)data_);
    free(data_);
  }
}

bool
sys::MutexImpl::tryacquire()
{
  return mtx_trylock((mtx_t*)data_) == thrd_success;
}

bool
sys::MutexImpl::acquire()
{
  return mtx_lock((mtx_t*)data_) == thrd_success;
}

bool
sys::MutexImpl::release()
{
  return mtx_unlock((mtx_t*)data_) == thrd_success;
}

}

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

namespace llvm {

sys::MutexImpl::MutexImpl(bool recursive)
{
    mtx_t mtx;
    if (recursive) mtx_init(&mtx, mtx_recursive);
    else mtx_init(&mtx, mtx_plain);
    _data = (void*)mtx;
}

sys::MutexImpl::~MutexImpl()
{
  mtx_t mtx = (mtx_t)_data;
  mtx_destroy(&mtx);
}

bool
sys::MutexImpl::acquire()
{
  mtx_t mtx = (mtx_t)_data;
  return mtx_lock(&mtx) == thrd_success;
}

bool
sys::MutexImpl::release()
{ 
  mtx_t mtx = (mtx_t)_data;
  return mtx_unlock(&mtx) == thrd_success;
}

bool
sys::MutexImpl::tryacquire()
{
  mtx_t mtx = (mtx_t)_data;
  return mtx_trylock(&mtx) == thrd_success;
}

}

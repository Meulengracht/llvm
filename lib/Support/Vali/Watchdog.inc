//===--- Vali/Watchdog.inc - Vali Watchdog Implementation -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides the generic Vali implementation of the Watchdog class.
//
//===----------------------------------------------------------------------===//

namespace llvm {
  namespace sys {
    Watchdog::Watchdog(unsigned int seconds) {
      //alarm(seconds);
    }

    Watchdog::~Watchdog() {
      //alarm(0);
    }
  }
}

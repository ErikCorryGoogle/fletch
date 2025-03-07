// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#ifndef FLETCH_ENABLE_FFI

#include "src/vm/ffi.h"
#include "src/vm/natives.h"
#include "src/shared/assert.h"

namespace fletch {

void ForeignFunctionInterface::Setup() {}

void ForeignFunctionInterface::TearDown() {}

void ForeignFunctionInterface::AddDefaultSharedLibrary(const char* library) {
  FATAL("fletch vm was built without FFI support.");
}

void* ForeignFunctionInterface::LookupInDefaultLibraries(const char* symbol) {
  UNIMPLEMENTED();
  return NULL;
}

DefaultLibraryEntry* ForeignFunctionInterface::libraries_ = NULL;
Mutex* ForeignFunctionInterface::mutex_ = NULL;

#define UNIMPLEMENTED_NATIVE(name) \
  NATIVE(name) {                   \
    UNIMPLEMENTED();               \
    return NULL;                   \
  }

UNIMPLEMENTED_NATIVE(ForeignLibraryLookup)
UNIMPLEMENTED_NATIVE(ForeignLibraryGetFunction)
UNIMPLEMENTED_NATIVE(ForeignLibraryBundlePath)
UNIMPLEMENTED_NATIVE(ForeignLibraryClose)

UNIMPLEMENTED_NATIVE(ForeignErrno)

}  // namespace fletch

#endif  // not FLETCH_ENABLE_FFI

// Copyright (c) 2014, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#include "src/shared/assert.h"

#include <stdarg.h>
#include <stdlib.h>

#include "src/shared/utils.h"
#include "src/shared/platform.h"

namespace fletch {
namespace DynamicAssertionHelper {

static void PrintError(const char* file, int line, const char* format,
                       const va_list& arguments) {
#ifdef FLETCH_ENABLE_PRINT_INTERCEPTORS
  // Print out the error.
  Print::Error("%s:%d: error: ", file, line);
  char buffer[KB];
  vsnprintf(buffer, sizeof(buffer), format, const_cast<va_list&>(arguments));
  Print::Error("%s\n", buffer);
#else
  fprintf(stderr, "%s:%d: error: ", file, line);
  vfprintf(stderr, format, arguments);
  fprintf(stderr, "\n");
#endif  // FLETCH_SUPPORT_PRINT_INTERCEPTORS
}

template <>
void Fail<ASSERT>(const char* file, int line, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  PrintError(file, line, format, arguments);
  va_end(arguments);
  Platform::ImmediateAbort();
}

template <>
void Fail<EXPECT>(const char* file, int line, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  PrintError(file, line, format, arguments);
  va_end(arguments);
  Platform::ScheduleAbort();
}

}  // namespace DynamicAssertionHelper
}  // namespace fletch

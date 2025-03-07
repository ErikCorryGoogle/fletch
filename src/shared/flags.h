// Copyright (c) 2014, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#ifndef SRC_SHARED_FLAGS_H_
#define SRC_SHARED_FLAGS_H_

#include "src/shared/assert.h"
#include "src/shared/globals.h"

namespace fletch {

// Flags provide access to commmand line flags.
//
// Syntax:
//   -Xname (equivalent to -Xname=true)
//   -Xname=<boolean>|<int>|<address>|<string>
//
// debug means the flag is ONLY available in the debug build.
// release means the flag is available in both the debug and release build.

#define FLAG_BOOLEAN(macro, name, value, doc) \
  macro(bool, Boolean, name, value, doc)
#define FLAG_INTEGER(macro, name, value, doc) \
  macro(int, Integer, name, value, doc)
#define FLAG_CSTRING(macro, name, value, doc) \
  macro(const char*, String, name, value, doc)

#define APPLY_TO_FLAGS(debug, release)                                    \
  FLAG_BOOLEAN(release, expose_gc, false,                                 \
               "Expose invoking GC to native call.")                      \
  FLAG_BOOLEAN(debug, validate_stack, false,                              \
               "Validate stack at each interperter step")                 \
  FLAG_BOOLEAN(release, unfold_program, false,                            \
               "Unfold the program before running")                       \
  FLAG_BOOLEAN(release, gc_on_delete, false,                              \
               "GC the heap at when terminating isolate")                 \
  FLAG_BOOLEAN(release, validate_heaps, false,                            \
               "Validate consistency of heaps.")                          \
  FLAG_BOOLEAN(debug, log_decoder, false, "Log decoding")                 \
  FLAG_BOOLEAN(debug, print_program_statistics, false,                    \
               "Print statistics about the program")                      \
  FLAG_BOOLEAN(release, print_heap_statistics, false,                     \
               "Print heap statistics before GC")                         \
  FLAG_BOOLEAN(release, verbose, false, "Verbose output")                 \
  FLAG_BOOLEAN(debug, print_flags, false, "Print flags")                  \
  FLAG_BOOLEAN(release, profile, false,                                   \
               "Profile the execution of the entire VM")                  \
  FLAG_INTEGER(release, profile_interval, 1000, "Profile interval in us") \
  FLAG_CSTRING(release, filter, NULL, "Filter string for unit testing")   \
  /* Temporary compiler flags */                                          \
  FLAG_BOOLEAN(release, trace_compiler, false, "")                        \
  FLAG_BOOLEAN(release, trace_library, false, "")

#ifdef DEBUG
#define DECLARE_DEBUG_FLAG(type, prefix, name, value, doc) static type name;
#else
#define DECLARE_DEBUG_FLAG(type, prefix, name, value, doc) \
  static const type name = value;
#endif

#define DECLARE_RELEASE_FLAG(type, prefix, name, value, doc) static type name;

class Flags {
 public:
  APPLY_TO_FLAGS(DECLARE_DEBUG_FLAG, DECLARE_RELEASE_FLAG)

  // Extract the flag values from the command line arguments.
  static void ExtractFromCommandLine(int* argc, char** argv);

  static char* executable() { return executable_; }

 private:
  static char* executable_;
};

}  // namespace fletch

#endif  // SRC_SHARED_FLAGS_H_

// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#if defined(FLETCH_TARGET_IA32) && defined(FLETCH_TARGET_OS_MACOS)

#include <stdio.h>
#include "src/vm/assembler.h"

namespace fletch {

#if defined(FLETCH_TARGET_ANDROID)
static const char* kPrefix = "";
#else
static const char* kPrefix = "_";
#endif

void Assembler::call(const char* name) {
  printf("\tcall %s%s\n", kPrefix, name);
}

void Assembler::j(Condition condition, const char* name) {
  const char* mnemonic = ConditionMnemonic(condition);
  printf("\tj%s %s%s\n", mnemonic, kPrefix, name);
}

void Assembler::jmp(const char* name) { printf("\tjmp %s%s\n", kPrefix, name); }

void Assembler::jmp(const char* name, Register index, ScaleFactor scale) {
  Print("jmp *%s%s(,%rl,%d)", kPrefix, name, index, 1 << scale);
}

void Assembler::Bind(const char* prefix, const char* name) {
  putchar('\n');
  printf("\t.globl %s%s%s\n", kPrefix, prefix, name);
  printf("%s%s%s:\n", kPrefix, prefix, name);
}

void Assembler::DefineLong(const char* name) {
  printf("\t.long %s%s\n", kPrefix, name);
}

void Assembler::LoadNative(Register reg, Register index) {
  Print("movl %skNativeTable(,%rl,4), %rl", kPrefix, index, reg);
}

}  // namespace fletch

#endif  // defined FLETCH_TARGET_IA32 && defined(FLETCH_TARGET_OS_MACOS)

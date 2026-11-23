/** @file
  Universal arm64 -> x86_64 translation snippets, in hybrid "hex and obj"
  form:

    - the hex form is the byte arrays below (mArm64Nop, mArm64Ret, ...);
    - the obj form is translations.o, assembled from translations.nasm by
      generate.sh - the x86_64 machine code that executes where the arm64
      form does.

  The translations are per arm64 instruction form, not per macOS version,
  so they are universal across the Apple Silicon arm64 macOS builds in the
  Golden Gate series (and any other Apple Silicon macOS built on these
  forms).  Each snippet is the x86_64 code that executes where the arm64
  form does; the branch snippets carry a zero rel32 that the consumer
  patches to the translated destination, as Stela's relocation machinery
  does.

  The OC_ARM64_X86_64_REG_MAP table gives the guest->host register map the
  universal snippets are written against; the guest is the arm64 state,
  the host the x86_64 state.

  Copyright (c) 2026, OpenCode. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OC_ARM64_X86_64_TRANSLATIONS_H
#define OC_ARM64_X86_64_TRANSLATIONS_H

#include <Uefi.h>

//
// Guest->host register map.  Golden Gate boot paths on Apple Silicon use
// the AArch64 register set (X0-X30 plus SP); the snippets in this package
// are written against the map below, which the consumer of the
// translations patches into any host encoding it uses.
//
typedef struct {
  CONST CHAR8   *Arm64Name;
  CONST CHAR8   *X86_64Name;
} OC_ARM64_X86_64_REG_MAP;

STATIC CONST OC_ARM64_X86_64_REG_MAP  mArm64ToX8664RegMap[] = {
  { "x0",  "rax" }, { "x1",  "rbx" }, { "x2",  "rcx" }, { "x3",  "rdx" },
  { "x4",  "rsi" }, { "x5",  "rdi" }, { "x6",  "r8" },  { "x7",  "r9" },
  { "x8",  "r10" }, { "x9",  "r11" }, { "x10", "r12" }, { "x11", "r13" },
  { "x12", "r14" }, { "x13", "r15" },
  { "sp",  "rsp" },
};

//
// A universal arm64 -> x86_64 translation: a guest arm64 instruction form
// and the x86_64 machine code that implements it.
//
typedef struct {
  CONST CHAR8   *Form;
  CONST UINT8   *Code;
  UINTN         CodeSize;
} OC_ARM64_X86_64_TRANSLATION;

//
// arm64: nop  ->  x86_64: nop
//
STATIC CONST UINT8  mArm64Nop[] = { 0x90 };

//
// arm64: ret  ->  x86_64: ret
//
STATIC CONST UINT8  mArm64Ret[] = { 0xC3 };

//
// arm64: b <imm26>  ->  x86_64: jmp rel32 (E9 + rel32, patch rel32 to the
// translated destination)
//
STATIC CONST UINT8  mArm64B[] = { 0xE9, 0x00, 0x00, 0x00, 0x00 };

//
// arm64: bl <imm26>  ->  x86_64: call rel32 (E8 + rel32)
//
STATIC CONST UINT8  mArm64BL[] = { 0xE8, 0x00, 0x00, 0x00, 0x00 };

//
// arm64: movz x0, #imm16, LSL #0  ->  x86_64: movabs rax, imm64
//        (48 B8 + imm64, the consumer sets imm64)
//
STATIC CONST UINT8  mArm64MovzX0[] = {
  0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

//
// arm64: mov x0, x1  ->  x86_64: mov rax, rbx
//
STATIC CONST UINT8  mArm64MovX0X1[] = { 0x48, 0x89, 0xD8 };

//
// arm64: ldr x0, [sp, #0x10]  ->  x86_64: mov rax, qword [rsp + 0x10]
//
STATIC CONST UINT8  mArm64LdrX0Sp[] = { 0x48, 0x8B, 0x44, 0x24, 0x10 };

//
// arm64: str x0, [sp, #0x10]  ->  x86_64: mov qword [rsp + 0x10], rax
//
STATIC CONST UINT8  mArm64StrX0Sp[] = { 0x48, 0x89, 0x44, 0x24, 0x10 };

STATIC CONST OC_ARM64_X86_64_TRANSLATION  mArm64ToX8664[] = {
  { "arm64 nop -> x86_64 nop",
    mArm64Nop, sizeof (mArm64Nop) },
  { "arm64 ret -> x86_64 ret",
    mArm64Ret, sizeof (mArm64Ret) },
  { "arm64 b <imm26> -> x86_64 jmp rel32",
    mArm64B, sizeof (mArm64B) },
  { "arm64 bl <imm26> -> x86_64 call rel32",
    mArm64BL, sizeof (mArm64BL) },
  { "arm64 movz x0, #imm16, LSL #0 -> x86_64 movabs rax, imm64",
    mArm64MovzX0, sizeof (mArm64MovzX0) },
  { "arm64 mov x0, x1 -> x86_64 mov rax, rbx",
    mArm64MovX0X1, sizeof (mArm64MovX0X1) },
  { "arm64 ldr x0, [sp, #0x10] -> x86_64 mov rax, qword [rsp + 0x10]",
    mArm64LdrX0Sp, sizeof (mArm64LdrX0Sp) },
  { "arm64 str x0, [sp, #0x10] -> x86_64 mov qword [rsp + 0x10], rax",
    mArm64StrX0Sp, sizeof (mArm64StrX0Sp) },
};

#endif // OC_ARM64_X86_64_TRANSLATIONS_H

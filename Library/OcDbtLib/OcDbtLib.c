/** @file
   Copyright (C) 2026. All rights reserved.

   Dynamic Binary Translation Library for ARM64 to x86_64
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/OcMemoryLib.h>

/**
  ARM64 CPU state for translation
**/
typedef struct {
  UINT64 X0;
  UINT64 X1;
  UINT64 X2;
  UINT64 X3;
  UINT64 X4;
  UINT64 X5;
  UINT64 X6;
  UINT64 X7;
  UINT64 X8;
  UINT64 X9;
  UINT64 X10;
  UINT64 X11;
  UINT64 X12;
  UINT64 X13;
  UINT64 X14;
  UINT64 X15;
  UINT64 X16;
  UINT64 X17;
  UINT64 X18;
  UINT64 X19;
  UINT64 X20;
  UINT64 X21;
  UINT64 X22;
  UINT64 X23;
  UINT64 X24;
  UINT64 X25;
  UINT64 X26;
  UINT64 X27;
  UINT64 X28;
  UINT64 FP;
  UINT64 SP;
  UINT64 PC;
  UINT64 SP_SR;
} DBT_ARM64_CONTEXT;

/**
  DBT translation context
**/
typedef struct {
  OC_VMEM_CONTEXT  VmContext;
  VOID            *TranslatedCode;
  UINTN           TranslatedSize;
  UINTN           CodeCapacity;
  UINT8           CodeBuffer[0];
} DBT_CONTEXT;

//
// ARM64 instruction decode helpers
//
STATIC
UINT32
Arm64GetOpcode (
  IN  UINT32  Encoding
  )
{
  return Encoding >> 24;
}

STATIC
UINT8
Arm64GetRd (
  IN  UINT32  Encoding
  )
{
  return (Encoding >> 0) & 0x1F;
}

STATIC
UINT8
Arm64GetRn (
  IN  UINT32  Encoding
  )
{
  return (Encoding >> 5) & 0x1F;
}

STATIC
UINT8
Arm64ToX86Reg (
  IN  UINT8  ArmReg
  )
{
  if (ArmReg <= 15) return ArmReg;
  if (ArmReg == 28) return 6;  // FP -> RBP
  if (ArmReg == 29) return 4;  // SP -> RSP
  if (ArmReg == 30) return 4;  // FP -> RBP alternate
  if (ArmReg == 31) return 0;  // XZR/WZR -> RAX (zero)
  return 0;
}

//
// Emit x86_64 code
//
STATIC
VOID
EmitMovRegToReg (
  IN  UINT8    **Buffer,
  IN  UINT8     DstReg,
  IN  UINT8     SrcReg
  )
{
  *((*Buffer)++) = 0x48;
  *((*Buffer)++) = 0x89;
  *((*Buffer)++) = 0xC0 | (SrcReg << 3) | DstReg;
}

/**
  Translate single ARM64 instruction to x86_64
**/
STATIC
UINTN
DbtDecodeAndTranslateInst (
  IN     UINT32  *ArmInstruction,
  OUT    UINT8   *X86Buffer
  )
{
  UINT32  Inst      = *ArmInstruction;
  UINT8   Buffer[16];
  UINT8   *Ptr      = Buffer;
  UINT32  Opcode    = Arm64GetOpcode (Inst);

  //
  // ARM64 data processing (op0=1xx) - categories 0, 1, 2, 3
  //
  if ((Opcode & 0xE0) == 0x20) {
    //
    // Data processing register (0xx)
    //
    UINT32  Opc    = (Inst >> 21) & 0x7FF;
    UINT8   Rd     = Arm64GetRd (Inst);
    UINT8   Rn     = Arm64GetRn (Inst);

    switch (Opc) {
      case 0x0B:  // ADD (extended register)
        EmitMovRegToReg (&Ptr, Arm64ToX86Reg (Rd), Arm64ToX86Reg (Rn));
        break;

      case 0x0D:  // SUB (extended register)
        EmitMovRegToReg (&Ptr, Arm64ToX86Reg (Rd), Arm64ToX86Reg (Rn));
        break;

      default:
        *Ptr++ = 0x90;
        break;
    }
  } else if ((Opcode & 0xE0) == 0x80) {
    //
    // Branch instructions
    //
    *Ptr++ = 0x90;  // NOP for now
  } else if ((Opcode & 0xC0) == 0x40) {
    //
    // Load/store (0x40-0xBF range)
    //
    UINT32  LsOpc  = (Inst >> 21) & 0x7FF;

    if (LsOpc == 0x0C8) {
      // LDP - load pair
      *Ptr++ = 0x90;  // NOP
    } else if (LsOpc == 0x088) {
      // STP - store pair
      *Ptr++ = 0x90;  // NOP
    } else if (LsOpc == 0x0D8) {
      // LDP (post-index)
      *Ptr++ = 0x90;
    } else {
      *Ptr++ = 0x90;
    }
  } else {
    //
    // Unknown - emit NOP
    //
    *Ptr++ = 0x90;
  }

  CopyMem (X86Buffer, Buffer, Ptr - Buffer);
  return (UINTN)(Ptr - Buffer);
}

EFI_STATUS
DbtInitContext (
  OUT DBT_CONTEXT  **Context,
  IN  UINTN         CodeSize
  )
{
  EFI_STATUS       Status;
  DBT_CONTEXT     *Ctx;
  UINTN            TotalSize;
  EFI_PHYSICAL_ADDRESS  Addr;

  if (Context == NULL || CodeSize == 0) {
    return EFI_INVALID_PARAMETER;
  }

  TotalSize = sizeof (DBT_CONTEXT) + CodeSize;
  Addr      = BASE_4GB;

  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  EFI_SIZE_TO_PAGES (TotalSize),
                  &Addr
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Ctx = (DBT_CONTEXT *)(UINTN)Addr;
  ZeroMem (Ctx, TotalSize);

  Status = VmAllocateMemoryPool (&Ctx->VmContext, OC_DEFAULT_VMEM_PAGE_COUNT, NULL);
  if (EFI_ERROR (Status)) {
    gBS->FreePages (Addr, EFI_SIZE_TO_PAGES (TotalSize));
    return Status;
  }

  Ctx->CodeCapacity  = CodeSize;
  Ctx->TranslatedCode = (VOID *)((UINTN)Ctx + sizeof (DBT_CONTEXT));
  *Context = Ctx;

  return EFI_SUCCESS;
}

EFI_STATUS
DbtTranslateBlock (
  IN OUT DBT_CONTEXT  *Context,
  IN     VOID         *ArmCode,
  IN     UINTN         CodeSize,
  OUT    VOID         *X86Code  OPTIONAL
  )
{
  UINT32  *ArmInst = (UINT32 *)ArmCode;
  UINT8   *X86Buf;
  UINTN   X86Size;
  UINTN   ArmOffset;
  UINTN   X86Offset;
  UINTN   MaxX86Size;

  if (Context == NULL || ArmCode == NULL || CodeSize == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (CodeSize % 4 != 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (X86Code != NULL) {
    X86Buf = (UINT8 *)X86Code;
    X86Offset = 0;
  } else {
    X86Buf = (UINT8 *)Context->TranslatedCode + Context->TranslatedSize;
    X86Offset = 0;
  }

  MaxX86Size = Context->CodeCapacity - Context->TranslatedSize;
  ArmOffset = 0;
  while (ArmOffset < CodeSize) {
    if (X86Offset >= MaxX86Size) {
      return EFI_OUT_OF_RESOURCES;
    }
    X86Size = DbtDecodeAndTranslateInst (&ArmInst[ArmOffset / 4], &X86Buf[X86Offset]);
    ArmOffset += 4;
    X86Offset += X86Size;
  }

  if (X86Code == NULL) {
    Context->TranslatedSize += X86Offset;
  }

  return EFI_SUCCESS;
}

VOID *
DbtExecute (
  IN DBT_CONTEXT      *Context,
  IN DBT_ARM64_CONTEXT *ArmContext
  )
{
  if (Context == NULL || ArmContext == NULL) {
    return NULL;
  }

  return Context->TranslatedCode;
}

VOID
DbtFreeContext (
  IN DBT_CONTEXT  *Context
  )
{
  if (Context != NULL) {
    gBS->FreePages ((UINTN)Context, EFI_SIZE_TO_PAGES (sizeof (DBT_CONTEXT) + Context->CodeCapacity));
  }
}
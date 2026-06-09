/** @file
  Copyright (C) 2026. All rights reserved.

  Dynamic Binary Translation Library for ARM64 to x86_64
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/OcMemoryLib.h>
#include <Library/OcDbtLib.h>

STATIC
EFI_STATUS
DbtDecodeAndTranslate (
  IN     UINT32  *ArmInstruction,
  OUT    UINT8   *X86Buffer,
  IN OUT UINTN   *X86Size
  )
{
  UINT32  Inst = *ArmInstruction;
  UINT32  Opc = (Inst >> 21) & 0x7FF;
  UINT32  Rd = Inst & 0x1F;
  UINT32  Rn = (Inst >> 5) & 0x1F;

  if (Opc == 0x0B) {
    X86Buffer[0] = 0x48;
    X86Buffer[1] = 0x89;
    X86Buffer[2] = 0x45;
    X86Buffer[3] = 0xF0 + (Rd & 0x7);
    *X86Size = 4;
    return EFI_SUCCESS;
  }

  if (Opc == 0x0D) {
    X86Buffer[0] = 0x48;
    X86Buffer[1] = 0x8B;
    X86Buffer[2] = 0x45;
    X86Buffer[3] = 0xF0 + (Rn & 0x7);
    *X86Size = 4;
    return EFI_SUCCESS;
  }

  X86Buffer[0] = 0x90;
  *X86Size = 1;
  return EFI_SUCCESS;
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
    DbtDecodeAndTranslate (&ArmInst[ArmOffset / 4], &X86Buf[X86Offset], &X86Size);
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
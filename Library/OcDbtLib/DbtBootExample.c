/** @file
   DbtBootExample — reference boot flow through OcDbtLib.

   Shows the minimum sequence a consumer needs to boot an ARM64 kernel on
   x86_64 via the DBT engine.  This file is documentation-only and is NOT
   part of the [Sources] list in OcDbtLib.inf; the real consumer lives in
   Platform/OpenDbvX64Dxe.

   Copyright (C) 2026. All rights reserved.

   SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcDbtLib.h>

//
// Kernel image already loaded by the caller, together with its Mach-O
// segment layout (virtual addresses vs file offsets) and entry point.
//
typedef struct {
  UINT64  *SegVmAddr;
  UINT64  *SegVmSize;
  UINT64  *SegFileOff;
  UINTN    SegCount;
  UINT8   *KernelBuffer;
  UINT64   EntryPoint;
  UINT64   InitialSp;
} DBT_EXAMPLE_KERNEL;

EFI_STATUS
EFIAPI
DbtBootExample (
  IN EFI_HANDLE            InstallerDevice,
  IN CONST CHAR16          *KernelPath,
  IN DBT_EXAMPLE_KERNEL    *Kernel
  )
{
  EFI_STATUS        Status;
  DBT_CONTEXT      *Ctx;
  DBT_ARM64_CONTEXT ArmState;
  VOID             *BootShimBuffer;

  //
  // 1. Create the translation context (1 MB code cache) and register the
  //    kernel for boot.  DbtInitContext also initialises the GIC and the
  //    standard Apple Silicon device set.
  //
  Ctx = NULL;
  Status = DbtInitContext (&Ctx, 0x100000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = DbtSetBootInfo (Ctx, InstallerDevice, KernelPath);
  if (EFI_ERROR (Status)) {
    DbtFreeContext (Ctx);
    return Status;
  }

  //
  // 2. Register the kernel segments so guest VAs resolve against the
  //    host-resident kernel image.
  //
  Status = DbtSetSegments (
             Ctx,
             Kernel->SegCount,
             Kernel->SegVmAddr,
             Kernel->SegVmSize,
             Kernel->SegFileOff,
             Kernel->KernelBuffer
             );
  if (EFI_ERROR (Status)) {
    DbtFreeContext (Ctx);
    return Status;
  }

  //
  // 3. Back the low "physical" window (early boot shim globals) with
  //    deterministic host memory.
  //
  BootShimBuffer = AllocateZeroPool (0x10000);
  if (BootShimBuffer != NULL) {
    DbtSetPhysWindow (Ctx, 0x1C0, 0x10000, BootShimBuffer);
  }

  //
  // 4. Set the initial ARM64 architectural state (identity-mapped boot).
  //
  ZeroMem (&ArmState, sizeof (ArmState));
  ArmState.PC     = Kernel->EntryPoint;
  ArmState.SP     = Kernel->InitialSp;
  ArmState.PSTATE = 0x5;                    // EL1h, DAIF masked

  //
  // 5. Translate the entry block and run.  The driver loop keeps calling
  //    DbtTranslateBlock + DbtExecute as the guest PC advances; cached
  //    blocks are re-entered without re-translation.
  //
  Status = DbtTranslateBlock (
             Ctx,
             Kernel->KernelBuffer + 0,      // guest entry bytes
             0x100,
             ArmState.PC,
             NULL
             );
  if (EFI_ERROR (Status)) {
    DbtFreeContext (Ctx);
    return Status;
  }

  DbtExecute (Ctx, &ArmState);

  DbtFreeContext (Ctx);
  return EFI_SUCCESS;
}

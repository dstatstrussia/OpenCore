/** @file
   DbtNvidiaGpuExample — reference NVIDIA GPU bring-up through the enhanced
   OcDbtLib surface.

   Demonstrates the nullmoth/nvidia-macos-driver inspired layer on top of
   the DBT engine:

     - PCI device registration (NVRM-style)
     - BAR1 relocation (ResizeGpuBars quirk concept)
     - BAR1 video-memory mapping
     - GSP firmware staging into BAR1 (rm_init_adapter concept)
     - HWRT-inspired scoreboards
     - Sleep/wake state save / restore

   This file is documentation-only and is NOT part of the [Sources] list in
   OcDbtLib.inf.

   Copyright (C) 2026. All rights reserved.

   SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcDbtLib.h>

//
// nullmoth driver: NVIDIA vendor ID; RTX 5060 device ID is used as an
// example (discover the real ID from PCI enumeration at runtime).
//
#define EXAMPLE_NVIDIA_VENDOR_ID  0x10DE
#define EXAMPLE_RTX_5060_DEVICE_ID 0x2A82

//
// Example quirks mask: ResizeGpuBars + ResizeAppleGpuBars (see the
// nvidia-macos-driver docs for the full set: csr-active-config, amfi, ...).
//
#define EXAMPLE_NVIDIA_QUIRKS \
  (DBT_QUIRK_RESIZE_GPU_BARS | DBT_QUIRK_RESIZE_APPLE_GPU_BARS)

EFI_STATUS
EFIAPI
DbtNvidiaGpuExample (
  IN EFI_HANDLE     InstallerDevice,
  IN CONST CHAR16  *KernelPath,
  IN CONST CHAR16  *GspFirmwarePath   OPTIONAL
  )
{
  EFI_STATUS       Status;
  DBT_CONTEXT     *Ctx;
  DBT_PCI_DEVICE  *Gpu;
  DBT_ARM64_CONTEXT ArmState;

  Ctx = NULL;
  Gpu = NULL;

  //
  // 1. Create the DBT context.
  //
  Status = DbtInitContext (&Ctx, 0x200000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 2. Apply the OpenCore quirks required by the NVIDIA bring-up path.
  //
  DbtApplyQuirks (Ctx, EXAMPLE_NVIDIA_QUIRKS);

  Status = DbtSetBootInfo (Ctx, InstallerDevice, KernelPath);
  if (EFI_ERROR (Status)) {
    DbtFreeContext (Ctx);
    return Status;
  }

  //
  // 3. Register the NVIDIA GPU (NVRM-style device claiming).
  //
  Status = DbtPciRegisterDevice (
             Ctx,
             EXAMPLE_NVIDIA_VENDOR_ID,
             EXAMPLE_RTX_5060_DEVICE_ID,
             0x03,        // Display class
             0x02,        // 3D controller subclass
             0x00,        // Programming interface
             0xA1,        // Revision
             0x00, 0x01, 0x00  // Bus 0, Device 1, Function 0
             );
  if (EFI_ERROR (Status)) {
    DbtFreeContext (Ctx);
    return Status;
  }

  Gpu = DbtPciFindDevice (Ctx, EXAMPLE_NVIDIA_VENDOR_ID, EXAMPLE_RTX_5060_DEVICE_ID);
  if (Gpu == NULL) {
    DbtFreeContext (Ctx);
    return EFI_NOT_FOUND;
  }

  //
  // 4. Relocate BAR1 away from the firmware console range (ResizeGpuBars)
  //    and map the host-backed video-memory aperture.
  //
  Status = DbtPciRelocateBar1 (Ctx, 0, 0xF000000000ULL, 0x10000000ULL);
  if (EFI_ERROR (Status)) {
    DbtFreeContext (Ctx);
    return Status;
  }

  Status = DbtGpuMapBar1VideoMemory (Ctx, 0, 0xF000000000ULL, 0x10000000ULL);
  if (EFI_ERROR (Status)) {
    DbtFreeContext (Ctx);
    return Status;
  }

  //
  // 5. Enable bus mastering / memory space on the emulated device.
  //
  DbtPciSetBusMaster (Ctx, 0, TRUE);
  DbtPciSetMemorySpace (Ctx, 0, TRUE);

  //
  // 6. Stage the GSP firmware into BAR1 (rm_init_adapter concept).  The
  //    image is read through the installer volume's SimpleFileSystem.
  //
  if (GspFirmwarePath != NULL) {
    Status = DbtGpuLoadGspFirmware (Ctx, 0, GspFirmwarePath);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "DbtNvidiaGpuExample: GSP load failed: %r\n", Status));
    }
  }

  //
  // 7. Create scoreboards for GPU / RT-core synchronisation (HWRT).
  //
  DbtScoreboardCreate (Ctx, L"GPU_COMMAND_COMPLETE");
  DbtScoreboardCreate (Ctx, L"RT_QUERY_COMPLETE");

  //
  // 8. Configure the loop budget (nullmoth translator loop_budget concept).
  //
  DbtSetLoopBudget (Ctx, DBT_DEFAULT_LOOP_BUDGET);

  //
  // 9. Boot the kernel.  DbtSetSegments/DbtSetPhysWindow/DbtTranslateBlock/
  //    DbtExecute follow the same flow as DbtBootExample; the kernel's NVRM
  //    will find the BAR1 aperture and GSP image prepared above.
  //
  ZeroMem (&ArmState, sizeof (ArmState));
  ArmState.PC     = 0;          // caller supplies the real entry point
  ArmState.SP     = 0xA5000000ULL;
  ArmState.PSTATE = 0x5;

  if (Gpu != NULL) {
    ArmState.GpuVendorId   = Gpu->VendorId;
    ArmState.GpuDeviceId   = Gpu->DeviceId;
    ArmState.GpuClassCode  = Gpu->ClassCode;
    ArmState.GpuRevisionId = Gpu->RevisionId;
    ArmState.GpuBar1Base   = Gpu->Bars[1];
    ArmState.GpuBar1Size   = Gpu->BarSizes[1];
  }

  DbtExecute (Ctx, &ArmState);

  //
  // 10. Sleep/wake round trip (NVRM-inspired): save, suspend, restore.
  //
  {
    VOID  *SavedState;
    UINTN  SavedSize;

    SavedState = NULL;
    SavedSize  = 0;

    DbtSuspend (Ctx);
    Status = DbtSaveState (Ctx, &SavedState, &SavedSize);
    if (!EFI_ERROR (Status)) {
      Status = DbtRestoreState (Ctx, SavedState, SavedSize);
      FreePool (SavedState);
    }
    DbtResume (Ctx);
  }

  DbtFreeContext (Ctx);
  return EFI_SUCCESS;
}

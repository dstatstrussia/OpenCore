/** @file
   OcDbtLib — enhanced additions.

   Dynamic Binary Translation Library for ARM64 to x86_64, extended with
   concepts ported from Stela (https://github.com/tylertms/Stela) and
   nullmoth/nvidia-macos-driver (https://github.com/nullmoth/nvidia-macos-driver):

     - PCI device registration and BAR management (NVRM-inspired)
     - BAR1 video-memory relocation, mirroring the ResizeGpuBars quirk
     - GSP firmware loading into BAR1 (NVRM rm_init_adapter concept)
     - NVIDIA GPU register (PMC/PBUS) emulation and BAR1-backed access
     - GPU compute / RT-core ray-query instruction emulation hooks
     - HWRT-inspired scoreboards for asynchronous GPU synchronisation
     - Sleep/wake state save / restore (NVRM-inspired)
     - OpenCore quirks integration, PCD wiring and profiling counters

   The original engine in OcDbtLib.c is kept intact; this file only adds
   the extended surface on top of the shared DBT_CONTEXT.

   Copyright (C) 2026. All rights reserved.

   SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/OcMemoryLib.h>
#include <Protocol/SimpleFileSystem.h>

//
// The internal header pulls in the public <Library/OcDbtLib.h> and defines
// the private DBT_CONTEXT layout, exactly like OcDbtLib.c does.
//
#include "OcDbtLib.h"

#define DBG(...)  DEBUG (__VA_ARGS__)

//
// PCI configuration command register bits
//
#define DBT_PCI_COMMAND_IO_SPACE     0x0001
#define DBT_PCI_COMMAND_MEMORY_SPACE 0x0002
#define DBT_PCI_COMMAND_BUS_MASTER   0x0004

//
// NVIDIA vendor ID and a minimal subset of the PMC/PBUS register space
// (mirrors the register windows the NVRM kext touches during adapter init)
//
#define NVIDIA_VENDOR_ID    0x10DE
#define NV_PMC_ENABLE       0x00000000
#define NV_PBUS_BAR1_BASE   0x00002100
#define NV_PBUS_BAR1_LIMIT  0x00002104

//
// Sleep/wake serialisation format
//
#define DBT_STATE_MAGIC     SIGNATURE_32 ('D', 'B', 'T', 'S')
#define DBT_STATE_VERSION   1

// =========== PCI device & BAR management (NVRM-inspired) ===========

EFI_STATUS
DbtPciRegisterDevice (
  IN DBT_CONTEXT  *Ctx,
  IN UINT16        VendorId,
  IN UINT16        DeviceId,
  IN UINT8         ClassCode,
  IN UINT8         SubClass,
  IN UINT8         ProgIf,
  IN UINT8         RevisionId,
  IN UINT8         Bus,
  IN UINT8         Device,
  IN UINT8         Function
  )
{
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || Ctx->PciDeviceCount >= DBT_MAX_PCI_DEVICES) {
    return EFI_OUT_OF_RESOURCES;
  }

  Pci = &Ctx->PciDevices[Ctx->PciDeviceCount++];
  ZeroMem (Pci, sizeof (*Pci));

  Pci->VendorId    = VendorId;
  Pci->DeviceId    = DeviceId;
  Pci->ClassCode   = ClassCode;
  Pci->SubClass    = SubClass;
  Pci->ProgIf      = ProgIf;
  Pci->RevisionId  = RevisionId;
  Pci->Bus         = Bus;
  Pci->Device      = Device;
  Pci->Function    = Function;
  Pci->HeaderType  = 0;

  DBG ((DEBUG_INFO, "DBT_PCI: registered %04x:%04x class=%02x.%02x.%02x rev=%02x at %02x:%02x.%x\n",
        VendorId, DeviceId, ClassCode, SubClass, ProgIf, RevisionId, Bus, Device, Function));

  return EFI_SUCCESS;
}

EFI_STATUS
DbtPciMapBar (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         DeviceIndex,
  IN UINT32        BarIndex,
  IN UINT64        PhysBase,
  IN UINTN         Size,
  IN VOID         *HostBuffer  OPTIONAL
  )
{
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || DeviceIndex >= Ctx->PciDeviceCount || BarIndex >= DBT_MAX_PCI_BARS) {
    return EFI_INVALID_PARAMETER;
  }

  Pci                = &Ctx->PciDevices[DeviceIndex];
  Pci->Bars[BarIndex]     = PhysBase;
  Pci->BarSizes[BarIndex] = Size;

  //
  // Mirror the mapping into the host-controller BAR window (the legacy
  // PcieBars[] array keeps four slots, matching the original layout).
  //
  if (BarIndex < 4 && Ctx->PcieBarCount < 4) {
    DBT_PCIE_BAR  *Bar = &Ctx->PcieBars[Ctx->PcieBarCount++];
    Bar->PhysBase    = PhysBase;
    Bar->Size        = Size;
    Bar->BarIndex    = BarIndex;
    Bar->Enabled     = TRUE;
    Bar->HostMapping = (UINT8 *)HostBuffer;
  }

  DBG ((DEBUG_INFO, "DBT_PCI: BAR%u mapped @0x%llx size=0x%x for device %u\n",
        BarIndex, PhysBase, (UINT32)Size, (UINT32)DeviceIndex));

  return EFI_SUCCESS;
}

EFI_STATUS
DbtPciRelocateBar1 (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         DeviceIndex,
  IN UINT64        NewBase,
  IN UINTN         NewSize
  )
{
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || DeviceIndex >= Ctx->PciDeviceCount) {
    return EFI_INVALID_PARAMETER;
  }

  Pci = &Ctx->PciDevices[DeviceIndex];

  //
  // ResizeGpuBars concept: move BAR1 (the video-memory aperture) away from
  // the firmware console range into a host-backed window of its own so the
  // guest kernel sees a clean, deterministic mapping.
  //
  if (Pci->Bar1HostBuffer != NULL) {
    FreePool (Pci->Bar1HostBuffer);
    Pci->Bar1HostBuffer = NULL;
  }

  Pci->Bar1HostBuffer = AllocateZeroPool (NewSize);
  if (Pci->Bar1HostBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Pci->Bars[1]        = NewBase;
  Pci->BarSizes[1]    = NewSize;
  Pci->Bar1Relocated  = TRUE;

  Ctx->ArmState.GpuBar1Base = NewBase;
  Ctx->ArmState.GpuBar1Size = NewSize;

  DBG ((DEBUG_INFO, "DBT_PCI: BAR1 relocated to 0x%llx size=0x%x for device %u\n",
        NewBase, (UINT32)NewSize, (UINT32)DeviceIndex));

  return EFI_SUCCESS;
}

DBT_PCI_DEVICE *
DbtPciGetDevice (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         Index
  )
{
  if (!Ctx || Index >= Ctx->PciDeviceCount) {
    return NULL;
  }

  return &Ctx->PciDevices[Index];
}

DBT_PCI_DEVICE *
DbtPciFindDevice (
  IN DBT_CONTEXT  *Ctx,
  IN UINT16        VendorId,
  IN UINT16        DeviceId
  )
{
  UINTN  Index;

  if (!Ctx) {
    return NULL;
  }

  for (Index = 0; Index < Ctx->PciDeviceCount; Index++) {
    if (Ctx->PciDevices[Index].VendorId == VendorId
        && (DeviceId == 0 || Ctx->PciDevices[Index].DeviceId == DeviceId)) {
      return &Ctx->PciDevices[Index];
    }
  }

  return NULL;
}

EFI_STATUS
DbtPciSetBusMaster (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         DeviceIndex,
  IN BOOLEAN       Enable
  )
{
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || DeviceIndex >= Ctx->PciDeviceCount) {
    return EFI_INVALID_PARAMETER;
  }

  Pci                        = &Ctx->PciDevices[DeviceIndex];
  Pci->BusMasterEnabled      = Enable;

  if (Enable) {
    Pci->Command |= DBT_PCI_COMMAND_BUS_MASTER;
  } else {
    Pci->Command &= (UINT16)~DBT_PCI_COMMAND_BUS_MASTER;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
DbtPciSetMemorySpace (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         DeviceIndex,
  IN BOOLEAN       Enable
  )
{
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || DeviceIndex >= Ctx->PciDeviceCount) {
    return EFI_INVALID_PARAMETER;
  }

  Pci                       = &Ctx->PciDevices[DeviceIndex];
  Pci->MemorySpaceEnabled   = Enable;

  if (Enable) {
    Pci->Command |= DBT_PCI_COMMAND_MEMORY_SPACE;
  } else {
    Pci->Command &= (UINT16)~DBT_PCI_COMMAND_MEMORY_SPACE;
  }

  return EFI_SUCCESS;
}

// =========== GPU emulation (NVRM rm_init_adapter-inspired) ===========

EFI_STATUS
DbtGpuMapBar1VideoMemory (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         DeviceIndex,
  IN UINT64        Base,
  IN UINTN         Size
  )
{
  EFI_STATUS       Status;
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || DeviceIndex >= Ctx->PciDeviceCount) {
    return EFI_INVALID_PARAMETER;
  }

  Pci = &Ctx->PciDevices[DeviceIndex];

  if (Pci->Bar1HostBuffer != NULL) {
    FreePool (Pci->Bar1HostBuffer);
    Pci->Bar1HostBuffer = NULL;
  }

  Pci->Bar1HostBuffer = AllocateZeroPool (Size);
  if (Pci->Bar1HostBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Pci->Bars[1]       = Base;
  Pci->BarSizes[1]   = Size;
  Pci->Bar1Relocated = TRUE;

  Ctx->ArmState.GpuBar1Base = Base;
  Ctx->ArmState.GpuBar1Size = Size;

  //
  // Expose the aperture as an MMIO trap so the hardware emulation layer
  // routes accesses through the host-backed buffer.
  //
  Status = DbtMmioRegister (Ctx, L"GPU BAR1 Video Memory", Base, Size, NULL, NULL);
  if (EFI_ERROR (Status)) {
    DBG ((DEBUG_WARN, "DBT_GPU: BAR1 MMIO registration failed: %r\n", Status));
  }

  DBG ((DEBUG_INFO, "DBT_GPU: BAR1 video memory mapped @0x%llx size=0x%x for device %u\n",
        Base, (UINT32)Size, (UINT32)DeviceIndex));

  return EFI_SUCCESS;
}

STATIC DBT_PCI_DEVICE *
DbtGpuFindActive (
  IN DBT_CONTEXT  *Ctx
  )
{
  UINTN  Index;

  if (!Ctx) {
    return NULL;
  }

  for (Index = 0; Index < Ctx->PciDeviceCount; Index++) {
    if (Ctx->PciDevices[Index].Bar1HostBuffer != NULL) {
      return &Ctx->PciDevices[Index];
    }
  }

  return DbtPciFindDevice (Ctx, NVIDIA_VENDOR_ID, 0);
}

UINT64
DbtGpuRegRead (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         DeviceIndex,
  IN UINT64        Offset
  )
{
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || DeviceIndex >= Ctx->PciDeviceCount) {
    return 0;
  }

  Pci = &Ctx->PciDevices[DeviceIndex];

  switch (Offset) {
    case NV_PMC_ENABLE:
      return 0x00000001;
    case NV_PBUS_BAR1_BASE:
      return Pci->Bars[1] & 0xFFFFFFFF;
    case NV_PBUS_BAR1_LIMIT:
      return (Pci->Bars[1] + Pci->BarSizes[1] - 1) & 0xFFFFFFFF;
    default:
      break;
  }

  //
  // Offsets inside the BAR1 aperture read from the host-backed video memory.
  //
  if (Pci->Bar1HostBuffer != NULL && Offset < Pci->BarSizes[1]) {
    UINT8  *Bar1 = Pci->Bar1HostBuffer;
    UINT64  Value;

    if (Offset + sizeof (UINT64) <= Pci->BarSizes[1]) {
      CopyMem (&Value, Bar1 + Offset, sizeof (UINT64));
      return Value;
    }
  }

  return 0;
}

VOID
DbtGpuRegWrite (
  IN DBT_CONTEXT  *Ctx,
  IN UINTN         DeviceIndex,
  IN UINT64        Offset,
  IN UINT64        Value
  )
{
  DBT_PCI_DEVICE  *Pci;

  if (!Ctx || DeviceIndex >= Ctx->PciDeviceCount) {
    return;
  }

  Pci = &Ctx->PciDevices[DeviceIndex];

  switch (Offset) {
    case NV_PMC_ENABLE:
      DBG ((DEBUG_INFO, "DBT_GPU: PMC_ENABLE write 0x%llx\n", Value));
      return;
    case NV_PBUS_BAR1_BASE:
      //
      // BAR1 base relocation request (ResizeGpuBars path)
      //
      if (Pci->Bar1Relocated) {
        Pci->Bars[1] = (Pci->Bars[1] & ~0xFFFFFFFFULL) | (Value & 0xFFFFFFFFULL);
        Ctx->ArmState.GpuBar1Base = Pci->Bars[1];
        DBG ((DEBUG_INFO, "DBT_GPU: BAR1 base updated to 0x%llx\n", Pci->Bars[1]));
      }
      return;
    default:
      break;
  }

  if (Pci->Bar1HostBuffer != NULL && Offset < Pci->BarSizes[1]) {
    UINT8  *Bar1 = Pci->Bar1HostBuffer;

    if (Offset + sizeof (UINT64) <= Pci->BarSizes[1]) {
      CopyMem (Bar1 + Offset, &Value, sizeof (UINT64));
    }
  }
}

EFI_STATUS
DbtGpuLoadGspFirmware (
  IN DBT_CONTEXT   *Ctx,
  IN UINTN          DeviceIndex,
  IN CONST CHAR16  *FirmwarePath
  )
{
  EFI_STATUS                       Status;
  DBT_PCI_DEVICE                  *Pci;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Root;
  EFI_FILE_PROTOCOL               *File;
  UINT8                           *FwBuffer;
  UINT64                           FwSize;
  UINTN                            ReadSize;

  if (!Ctx || !FirmwarePath || DeviceIndex >= Ctx->PciDeviceCount) {
    return EFI_INVALID_PARAMETER;
  }

  Pci = &Ctx->PciDevices[DeviceIndex];

  if (Ctx->InstallerDevice == NULL) {
    return EFI_NOT_READY;
  }

  if (Pci->Bar1HostBuffer == NULL) {
    DBG ((DEBUG_WARN, "DBT_GSP: BAR1 video memory not mapped for device %u\n", (UINT32)DeviceIndex));
    return EFI_NOT_READY;
  }

  Status = gBS->HandleProtocol (
                  Ctx->InstallerDevice,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Root->Open (Root, &File, (CHAR16 *)FirmwarePath, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Root->Close (Root);
    return Status;
  }

  //
  // Determine the firmware image size.
  //
  Status = File->SetPosition (File, 0xFFFFFFFFFFFFFFFFULL);
  if (!EFI_ERROR (Status)) {
    Status = File->GetPosition (File, &FwSize);
  }
  if (EFI_ERROR (Status)) {
    File->Close (File);
    Root->Close (Root);
    return Status;
  }
  File->SetPosition (File, 0);

  if (FwSize > Pci->BarSizes[1]) {
    DBG ((DEBUG_WARN, "DBT_GSP: firmware %u bytes exceeds BAR1 0x%x\n",
          (UINT32)FwSize, (UINT32)Pci->BarSizes[1]));
    File->Close (File);
    Root->Close (Root);
    return EFI_BUFFER_TOO_SMALL;
  }

  FwBuffer = AllocatePool ((UINTN)FwSize);
  if (FwBuffer == NULL) {
    File->Close (File);
    Root->Close (Root);
    return EFI_OUT_OF_RESOURCES;
  }

  ReadSize = (UINTN)FwSize;
  Status   = File->Read (File, &ReadSize, FwBuffer);
  File->Close (File);
  Root->Close (Root);

  if (EFI_ERROR (Status)) {
    FreePool (FwBuffer);
    return Status;
  }

  FwSize = ReadSize;

  //
  // Stage the GSP image into BAR1 so the guest NVRM sees it during
  // rm_init_adapter.
  //
  CopyMem (Pci->Bar1HostBuffer, FwBuffer, (UINTN)FwSize);
  FreePool (FwBuffer);

  //
  // Publish the GPU identity into the ARM64 state (mirrors what the NVRM
  // kext learns from PCI config space).
  //
  Ctx->ArmState.GpuVendorId   = Pci->VendorId;
  Ctx->ArmState.GpuDeviceId   = Pci->DeviceId;
  Ctx->ArmState.GpuClassCode  = Pci->ClassCode;
  Ctx->ArmState.GpuRevisionId = Pci->RevisionId;
  Ctx->ArmState.GpuBar1Base   = Pci->Bars[1];
  Ctx->ArmState.GpuBar1Size   = Pci->BarSizes[1];

  DBG ((DEBUG_INFO, "DBT_GSP: GSP firmware loaded to BAR1 @0x%llx, %u bytes\n",
        Pci->Bars[1], (UINT32)FwSize));

  return EFI_SUCCESS;
}

// =========== GPU compute / RT-core ray-query emulation hooks ===========

BOOLEAN
DbtEmulateGpuCompute (
  IN OUT DBT_ARM64_STATE  *State,
  IN     UINT32            Inst,
  IN     DBT_CONTEXT      *Ctx
  )
{
  DBT_PCI_DEVICE  *Gpu;
  UINT8            Rd;
  UINT8            Rn;
  UINT8            Rm;
  UINT64           GpuVa;

  if (!State || !Ctx) {
    return FALSE;
  }

  Gpu = DbtGpuFindActive (Ctx);
  if (Gpu == NULL || Gpu->Bar1HostBuffer == NULL) {
    return FALSE;
  }

  Rd = (UINT8)(Inst & 0x1F);
  Rn = (UINT8)((Inst >> 5) & 0x1F);
  Rm = (UINT8)((Inst >> 16) & 0x1F);

  //
  // Address-calculation pattern: translate a guest GPU virtual address
  // through the BAR1 aperture into the host-backed video memory.
  //
  GpuVa = State->X[Rn] + State->X[Rm];

  if (State->GpuBar1Base != 0) {
    UINT64  Offset = GpuVa - State->GpuBar1Base;

    if (Offset < State->GpuBar1Size) {
      State->X[Rd] = (UINT64)(UINTN)(Gpu->Bar1HostBuffer + Offset);

      if ((Ctx->TranslateFlags & DBT_TRANSLATE_FLAG_VERBOSE) != 0) {
        DBG ((DEBUG_INFO, "DBT_GPU: BAR1 access va=0x%llx offset=0x%llx\n", GpuVa, Offset));
      }
      return TRUE;
    }
  }

  return FALSE;
}

BOOLEAN
DbtEmulateRayQuery (
  IN OUT DBT_ARM64_STATE  *State,
  IN     UINT32            Inst,
  IN     DBT_CONTEXT      *Ctx
  )
{
  UINT8  Rd;

  if (!State || !Ctx || Ctx->ScoreboardCount == 0) {
    return FALSE;
  }

  Rd = (UINT8)(Inst & 0x1F);

  //
  // HWRT-inspired region: issue the query against a scoreboard and let the
  // caller drain it.  A full BVH traversal is future work; the reference
  // result is a miss (all ones) so software traversal stays authoritative
  // until the hardware path is validated, mirroring the staged approach in
  // the nullmoth driver (HWRT off until measured).
  //
  DbtScoreboardSignal (Ctx, 0);

  State->X[Rd] = 0xFFFFFFFFFFFFFFFFULL;

  if ((Ctx->TranslateFlags & DBT_TRANSLATE_FLAG_VERBOSE) != 0) {
    DBG ((DEBUG_INFO, "DBT_HWRT: ray query issued, scoreboard=0\n"));
  }

  return TRUE;
}

// =========== Scoreboard synchronisation (HWRT-inspired) ===========

DBT_SCOREBOARD *
DbtScoreboardCreate (
  IN DBT_CONTEXT   *Ctx,
  IN CONST CHAR16  *Name
  )
{
  DBT_SCOREBOARD  *Sb;

  if (!Ctx || !Name || Ctx->ScoreboardCount >= DBT_MAX_SCOREBOARDS) {
    return NULL;
  }

  Sb = &Ctx->Scoreboards[Ctx->ScoreboardCount++];
  ZeroMem (Sb, sizeof (*Sb));
  Sb->Active = TRUE;
  Sb->Name   = AllocateCopyPool (StrSize (Name), (VOID *)Name);

  DBG ((DEBUG_INFO, "DBT: scoreboard created '%s'\n", Name));

  return Sb;
}

VOID
DbtScoreboardSignal (
  IN DBT_CONTEXT  *Ctx,
  IN UINT32        ScoreboardIndex
  )
{
  if (!Ctx || ScoreboardIndex >= Ctx->ScoreboardCount) {
    return;
  }

  Ctx->Scoreboards[ScoreboardIndex].Value++;
  Ctx->Scoreboards[ScoreboardIndex].SignalMask |= 1ULL << (ScoreboardIndex % 64);
}

VOID
DbtScoreboardWait (
  IN DBT_CONTEXT  *Ctx,
  IN UINT32        ScoreboardIndex,
  IN UINT64        Timeout
  )
{
  UINT64  Iter;

  if (!Ctx || ScoreboardIndex >= Ctx->ScoreboardCount) {
    return;
  }

  //
  // Bounded spin: firmware-stage code must never wait forever.
  //
  for (Iter = 0; Iter < Timeout; Iter++) {
    if (Ctx->Scoreboards[ScoreboardIndex].SignalMask != 0) {
      return;
    }
  }

  DBG ((DEBUG_WARN, "DBT: scoreboard %u wait timeout after %llu iters\n",
        ScoreboardIndex, Timeout));
}

BOOLEAN
DbtScoreboardSignaled (
  IN DBT_CONTEXT  *Ctx,
  IN UINT32        ScoreboardIndex
  )
{
  if (!Ctx || ScoreboardIndex >= Ctx->ScoreboardCount) {
    return FALSE;
  }

  return Ctx->Scoreboards[ScoreboardIndex].SignalMask != 0;
}

// =========== Sleep/wake support (NVRM-inspired) ===========

EFI_STATUS
DbtSaveState (
  IN DBT_CONTEXT  *Ctx,
  OUT VOID       **Buffer,
  OUT UINTN       *BufferSize
  )
{
  UINTN   PciSize;
  UINTN   SbSize;
  UINTN   Total;
  UINT8  *Buf;
  UINT8  *Ptr;
  UINTN   Index;
  UINT32  Magic;
  UINT32  Version;

  if (!Ctx || !Buffer || !BufferSize) {
    return EFI_INVALID_PARAMETER;
  }

  PciSize = Ctx->PciDeviceCount * sizeof (DBT_PCI_DEVICE);
  SbSize  = Ctx->ScoreboardCount * sizeof (DBT_SCOREBOARD);
  Total   = sizeof (UINT32) * 2 + sizeof (UINTN)
            + sizeof (DBT_ARM64_STATE)
            + sizeof (Ctx->SysRegs)
            + sizeof (UINT32) * 3
            + sizeof (UINT64) * 5
            + sizeof (UINTN) + PciSize
            + sizeof (UINTN) + SbSize;

  Buf = AllocatePool (Total);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Ptr = Buf;

  Magic   = DBT_STATE_MAGIC;
  Version = DBT_STATE_VERSION;

  CopyMem (Ptr, &Magic, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (Ptr, &Version, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (Ptr, &Total, sizeof (UINTN));
  Ptr += sizeof (UINTN);

  CopyMem (Ptr, &Ctx->ArmState, sizeof (DBT_ARM64_STATE));
  Ptr += sizeof (DBT_ARM64_STATE);
  CopyMem (Ptr, Ctx->SysRegs, sizeof (Ctx->SysRegs));
  Ptr += sizeof (Ctx->SysRegs);

  CopyMem (Ptr, &Ctx->TranslateFlags, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (Ptr, &Ctx->Quirks, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (Ptr, &Ctx->DefaultLoopBudget, sizeof (UINT32));
  Ptr += sizeof (UINT32);

  CopyMem (Ptr, &Ctx->TotalTranslatedBlocks, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (Ptr, &Ctx->TotalExecutedBlocks, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (Ptr, &Ctx->CacheHits, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (Ptr, &Ctx->CacheMisses, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (Ptr, &Ctx->LoopBudgetExhausted, sizeof (UINT64));
  Ptr += sizeof (UINT64);

  CopyMem (Ptr, &Ctx->PciDeviceCount, sizeof (UINTN));
  Ptr += sizeof (UINTN);
  for (Index = 0; Index < Ctx->PciDeviceCount; Index++) {
    DBT_PCI_DEVICE  Local = Ctx->PciDevices[Index];
    //
    // Host allocations are not carried across sleep; the mapping must be
    // re-established after wake (NVRM re-initialises the BAR window too).
    //
    Local.Bar1HostBuffer = NULL;
    CopyMem (Ptr, &Local, sizeof (DBT_PCI_DEVICE));
    Ptr += sizeof (DBT_PCI_DEVICE);
  }

  CopyMem (Ptr, &Ctx->ScoreboardCount, sizeof (UINTN));
  Ptr += sizeof (UINTN);
  if (SbSize != 0) {
    CopyMem (Ptr, Ctx->Scoreboards, SbSize);
    Ptr += SbSize;
  }

  *Buffer          = Buf;
  *BufferSize      = Total;
  Ctx->SavedState     = Buf;
  Ctx->SavedStateSize = Total;

  DBG ((DEBUG_INFO, "DBT: state saved, %u bytes\n", (UINT32)Total));

  return EFI_SUCCESS;
}

EFI_STATUS
DbtRestoreState (
  IN DBT_CONTEXT  *Ctx,
  IN VOID         *Buffer,
  IN UINTN         BufferSize
  )
{
  UINT32  Magic;
  UINT32  Version;
  UINTN   Total;
  UINT8  *Ptr;
  UINTN   Index;
  UINTN   PciCount;
  UINTN   SbCount;

  if (!Ctx || !Buffer) {
    return EFI_INVALID_PARAMETER;
  }

  Ptr = (UINT8 *)Buffer;

  CopyMem (&Magic, Ptr, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (&Version, Ptr, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (&Total, Ptr, sizeof (UINTN));
  Ptr += sizeof (UINTN);

  if (Magic != DBT_STATE_MAGIC || Version != DBT_STATE_VERSION) {
    DBG ((DEBUG_ERROR, "DBT: state restore rejected (magic/version)\n"));
    return EFI_INCOMPATIBLE_VERSION;
  }

  if (Total != BufferSize) {
    DBG ((DEBUG_ERROR, "DBT: state restore size mismatch %u != %u\n",
          (UINT32)Total, (UINT32)BufferSize));
    return EFI_BAD_BUFFER_SIZE;
  }

  CopyMem (&Ctx->ArmState, Ptr, sizeof (DBT_ARM64_STATE));
  Ptr += sizeof (DBT_ARM64_STATE);
  CopyMem (Ctx->SysRegs, Ptr, sizeof (Ctx->SysRegs));
  Ptr += sizeof (Ctx->SysRegs);

  CopyMem (&Ctx->TranslateFlags, Ptr, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (&Ctx->Quirks, Ptr, sizeof (UINT32));
  Ptr += sizeof (UINT32);
  CopyMem (&Ctx->DefaultLoopBudget, Ptr, sizeof (UINT32));
  Ptr += sizeof (UINT32);

  CopyMem (&Ctx->TotalTranslatedBlocks, Ptr, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (&Ctx->TotalExecutedBlocks, Ptr, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (&Ctx->CacheHits, Ptr, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (&Ctx->CacheMisses, Ptr, sizeof (UINT64));
  Ptr += sizeof (UINT64);
  CopyMem (&Ctx->LoopBudgetExhausted, Ptr, sizeof (UINT64));
  Ptr += sizeof (UINT64);

  CopyMem (&PciCount, Ptr, sizeof (UINTN));
  Ptr += sizeof (UINTN);
  if (PciCount <= DBT_MAX_PCI_DEVICES) {
    for (Index = 0; Index < PciCount; Index++) {
      CopyMem (&Ctx->PciDevices[Index], Ptr, sizeof (DBT_PCI_DEVICE));
      Ptr += sizeof (DBT_PCI_DEVICE);
      Ctx->PciDevices[Index].Bar1HostBuffer = NULL;
    }
    Ctx->PciDeviceCount = PciCount;
  } else {
    return EFI_BAD_BUFFER_SIZE;
  }

  CopyMem (&SbCount, Ptr, sizeof (UINTN));
  Ptr += sizeof (UINTN);
  if (SbCount <= DBT_MAX_SCOREBOARDS) {
    if (SbCount != 0) {
      CopyMem (Ctx->Scoreboards, Ptr, SbCount * sizeof (DBT_SCOREBOARD));
      Ptr += SbCount * sizeof (DBT_SCOREBOARD);
    }
    Ctx->ScoreboardCount = SbCount;
  } else {
    return EFI_BAD_BUFFER_SIZE;
  }

  Ctx->Suspended = FALSE;

  DBG ((DEBUG_INFO, "DBT: state restored (%u bytes)\n", (UINT32)BufferSize));

  return EFI_SUCCESS;
}

EFI_STATUS
DbtSuspend (
  IN DBT_CONTEXT  *Ctx
  )
{
  if (!Ctx) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx->Suspended = TRUE;
  DBG ((DEBUG_INFO, "DBT: context suspended\n"));

  return EFI_SUCCESS;
}

EFI_STATUS
DbtResume (
  IN DBT_CONTEXT  *Ctx
  )
{
  if (!Ctx) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx->Suspended = FALSE;
  DBG ((DEBUG_INFO, "DBT: context resumed\n"));

  return EFI_SUCCESS;
}

// =========== OpenCore quirks integration ===========

VOID
DbtApplyQuirks (
  IN DBT_CONTEXT  *Ctx,
  IN UINT32        Quirks
  )
{
  if (!Ctx) {
    return;
  }

  Ctx->Quirks = Quirks;

  if ((Quirks & DBT_QUIRK_RESIZE_GPU_BARS) != 0) {
    DBG ((DEBUG_INFO, "DBT: ResizeGpuBars quirk enabled - BAR1 will be relocated\n"));
  }
  if ((Quirks & DBT_QUIRK_RESIZE_APPLE_GPU_BARS) != 0) {
    DBG ((DEBUG_INFO, "DBT: ResizeAppleGpuBars quirk enabled\n"));
  }
  if ((Quirks & DBT_QUIRK_AMFI_DISABLE) != 0) {
    DBG ((DEBUG_INFO, "DBT: AMFI disable quirk - unsigned translation allowed\n"));
  }
  if ((Quirks & DBT_QUIRK_SIP_DISABLE) != 0) {
    DBG ((DEBUG_INFO, "DBT: SIP disable quirk - full system access\n"));
  }
  if ((Quirks & DBT_QUIRK_NV_OFF) != 0) {
    DBG ((DEBUG_INFO, "DBT: NVIDIA driver disabled (-nvoff)\n"));
  }
  if ((Quirks & DBT_QUIRK_NV_KMS_NO_SMOOTH) != 0) {
    DBG ((DEBUG_INFO, "DBT: NV KMS smoothing disabled (-nvkmsnosmooth)\n"));
  }
}

UINT32
DbtGetQuirks (
  IN DBT_CONTEXT  *Ctx
  )
{
  return Ctx != NULL ? Ctx->Quirks : 0;
}

// =========== Translation options, profiling, diagnostics ===========

VOID
DbtSetLoopBudget (
  IN DBT_CONTEXT  *Ctx,
  IN UINT32        Budget
  )
{
  if (Ctx != NULL) {
    Ctx->DefaultLoopBudget = Budget;
  }
}

VOID
DbtSetVerbose (
  IN DBT_CONTEXT  *Ctx,
  IN BOOLEAN       Enable
  )
{
  if (Ctx != NULL) {
    if (Enable) {
      Ctx->TranslateFlags |= DBT_TRANSLATE_FLAG_VERBOSE;
    } else {
      Ctx->TranslateFlags &= (UINT32)~DBT_TRANSLATE_FLAG_VERBOSE;
    }
  }
}

VOID
DbtSetTraceMem (
  IN DBT_CONTEXT  *Ctx,
  IN BOOLEAN       Enable
  )
{
  if (Ctx != NULL) {
    if (Enable) {
      Ctx->TranslateFlags |= DBT_TRANSLATE_FLAG_TRACE_MEM;
    } else {
      Ctx->TranslateFlags &= (UINT32)~DBT_TRANSLATE_FLAG_TRACE_MEM;
    }
  }
}

VOID
DbtApplyPcds (
  IN DBT_CONTEXT  *Ctx
  )
{
  UINT32  Quirks;

  if (!Ctx) {
    return;
  }

  if (FixedPcdGetBool (PcdOcDbtVerbose)) {
    Ctx->TranslateFlags |= DBT_TRANSLATE_FLAG_VERBOSE;
  }
  if (FixedPcdGetBool (PcdOcDbtTraceMem)) {
    Ctx->TranslateFlags |= DBT_TRANSLATE_FLAG_TRACE_MEM;
  }

  Ctx->DefaultLoopBudget = FixedPcdGet32 (PcdOcDbtLoopBudget);

  Quirks = FixedPcdGet32 (PcdOcDbtQuirks);
  if (Quirks != 0) {
    DbtApplyQuirks (Ctx, Quirks);
  }

  DBG ((DEBUG_INFO, "DBT: PCDs applied (gpu=%d ray=%d pac=%d sve=%d budget=0x%x)\n",
        (int)FixedPcdGetBool (PcdOcDbtEnableGpu),
        (int)FixedPcdGetBool (PcdOcDbtEnableRayQuery),
        (int)FixedPcdGetBool (PcdOcDbtEnablePac),
        (int)FixedPcdGetBool (PcdOcDbtEnableSve),
        FixedPcdGet32 (PcdOcDbtLoopBudget)));
}

VOID
DbtGetStats (
  IN DBT_CONTEXT  *Ctx,
  OUT UINT64      *TotalTranslatedBlocks  OPTIONAL,
  OUT UINT64      *TotalExecutedBlocks    OPTIONAL,
  OUT UINT64      *CacheHits              OPTIONAL,
  OUT UINT64      *CacheMisses            OPTIONAL,
  OUT UINT64      *LoopBudgetExhausted    OPTIONAL
  )
{
  if (!Ctx) {
    return;
  }

  if (TotalTranslatedBlocks != NULL) {
    *TotalTranslatedBlocks = Ctx->TotalTranslatedBlocks;
  }
  if (TotalExecutedBlocks != NULL) {
    *TotalExecutedBlocks = Ctx->TotalExecutedBlocks;
  }
  if (CacheHits != NULL) {
    *CacheHits = Ctx->CacheHits;
  }
  if (CacheMisses != NULL) {
    *CacheMisses = Ctx->CacheMisses;
  }
  if (LoopBudgetExhausted != NULL) {
    *LoopBudgetExhausted = Ctx->LoopBudgetExhausted;
  }
}

VOID
DbtDumpPciDevices (
  IN DBT_CONTEXT  *Ctx
  )
{
  UINTN  Index;
  UINTN  Bar;

  if (!Ctx) {
    return;
  }

  DBG ((DEBUG_INFO, "DBT_PCI: %u PCI devices registered\n", Ctx->PciDeviceCount));

  for (Index = 0; Index < Ctx->PciDeviceCount; Index++) {
    DBT_PCI_DEVICE  *P = &Ctx->PciDevices[Index];

    DBG ((DEBUG_INFO, "  [%u] %04x:%04x class=%02x.%02x.%02x rev=%02x at %02x:%02x.%x cmd=0x%04x\n",
          Index, P->VendorId, P->DeviceId, P->ClassCode, P->SubClass, P->ProgIf,
          P->RevisionId, P->Bus, P->Device, P->Function, P->Command));

    for (Bar = 0; Bar < DBT_MAX_PCI_BARS; Bar++) {
      if (P->Bars[Bar] != 0) {
        DBG ((DEBUG_INFO, "      BAR%u: base=0x%llx size=0x%x\n",
              Bar, P->Bars[Bar], (UINT32)P->BarSizes[Bar]));
      }
    }
  }
}

VOID
DbtDumpScoreboards (
  IN DBT_CONTEXT  *Ctx
  )
{
  UINTN  Index;

  if (!Ctx) {
    return;
  }

  DBG ((DEBUG_INFO, "DBT: %u scoreboards\n", Ctx->ScoreboardCount));

  for (Index = 0; Index < Ctx->ScoreboardCount; Index++) {
    DBG ((DEBUG_INFO, "  [%u] %s value=%llu signaled=%d\n",
          Index, Ctx->Scoreboards[Index].Name, Ctx->Scoreboards[Index].Value,
          Ctx->Scoreboards[Index].SignalMask != 0));
  }
}

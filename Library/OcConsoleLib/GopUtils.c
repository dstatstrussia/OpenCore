/** @file
  GOP buffer and pixel size utility methods, and GOP burst mode caching code.

  Copyright (C) 2023, Mike Beaton. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "OcConsoleLibInternal.h"

#include <Base.h>
#include <Uefi/UefiBaseType.h>

#include <IndustryStandard/ProcessorInfo.h>
#include <IndustryStandard/VirtualMemory.h>

#include <Library/BaseLib.h>
#include <Library/BaseOverflowLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MtrrLib.h>
#include <Library/OcMemoryLib.h>
#include <Library/OcStringLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define PAT_INDEX_TO_CHANGE  7

EFI_STATUS
OcGopModeBytesPerPixel (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE  *Mode,
  OUT UINTN                              *BytesPerPixel
  )
{
  UINT32  MergedMasks;

  if (  (Mode == NULL)
     || (Mode->Info == NULL))
  {
    ASSERT (
      (Mode != NULL)
           && (Mode->Info != NULL)
      );
    return EFI_UNSUPPORTED;
  }

  //
  // This can occur without PixelBltOnly, including in rotated DirectGopRendering -
  // see comment about PixelFormat in ConsoleGop.c RotateMode method.
  //
  if (Mode->FrameBufferBase == 0ULL) {
    return EFI_UNSUPPORTED;
  }

  switch (Mode->Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
    case PixelBlueGreenRedReserved8BitPerColor:
      *BytesPerPixel = sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      return EFI_SUCCESS;

    case PixelBitMask:
      break;

    case PixelBltOnly:
      return EFI_UNSUPPORTED;

    default:
      return EFI_INVALID_PARAMETER;
  }

  //
  // Merge the masks with bitwise OR. Logical OR would collapse every
  // non-zero mask set to 1, making HighBitSet32 below return 0 and thus
  // computing a zero bytes-per-pixel value. Legacy GOPs (BiosVideo) report
  // PixelBitMask modes, and a zero size makes all of them look unusable.
  //
  MergedMasks = Mode->Info->PixelInformation.RedMask
                | Mode->Info->PixelInformation.GreenMask
                | Mode->Info->PixelInformation.BlueMask
                | Mode->Info->PixelInformation.ReservedMask;

  if (MergedMasks == 0) {
    return EFI_INVALID_PARAMETER;
  }

  *BytesPerPixel = (UINT32)((HighBitSet32 (MergedMasks) + 7) / 8);

  return EFI_SUCCESS;
}

EFI_STATUS
OcGopModeSafeFrameBufferSize (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE  *Mode,
  OUT UINTN                              *FrameBufferSize
  )
{
  EFI_STATUS  Status;
  UINTN       BytesPerPixel;

  if (  (Mode == NULL)
     || (Mode->Info == NULL))
  {
    ASSERT (
      (Mode != NULL)
           && (Mode->Info != NULL)
      );
    return EFI_UNSUPPORTED;
  }

  Status = OcGopModeBytesPerPixel (Mode, &BytesPerPixel);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (BaseOverflowTriMulUN (
        Mode->Info->PixelsPerScanLine,
        Mode->Info->VerticalResolution,
        BytesPerPixel,
        FrameBufferSize
        ))
  {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
OcGopModeUsable (
  IN  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info,
  OUT UINTN                                 *FrameBufferSize
  )
{
  UINTN   Size;
  UINTN   BytesPerPixel;
  UINTN   ScanLine;
  UINT32  MergedMasks;

  if (  (Info == NULL)
     || (FrameBufferSize == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Info->PixelFormat == PixelBltOnly) {
    //
    // BltOnly modes carry no frame buffer, so nothing to validate; they
    // are accepted, as before.
    //
    *FrameBufferSize = 0;
    return EFI_SUCCESS;
  }

  switch (Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
    case PixelBlueGreenRedReserved8BitPerColor:
      BytesPerPixel = sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
      break;

    case PixelBitMask:
      //
      // Bitwise OR is essential here: with logical OR the merged value is
      // always 1 for any non-zero mask, so HighBitSet32 returns 0 and the
      // computed bytes-per-pixel (and the frame buffer size) becomes zero.
      // That makes every PixelBitMask mode look unusable, which is exactly
      // the mode format reported by the BiosVideo GOP on legacy systems.
      //
      MergedMasks = Info->PixelInformation.RedMask
                    | Info->PixelInformation.GreenMask
                    | Info->PixelInformation.BlueMask
                    | Info->PixelInformation.ReservedMask;
      if (MergedMasks == 0) {
        return EFI_INVALID_PARAMETER;
      }

      BytesPerPixel = (UINTN)((HighBitSet32 (MergedMasks) + 7) / 8);
      if (BytesPerPixel == 0) {
        return EFI_UNSUPPORTED;
      }

      break;

    default:
      return EFI_UNSUPPORTED;
  }

  if (  (Info->HorizontalResolution == 0)
     || (Info->VerticalResolution == 0))
  {
    return EFI_NOT_FOUND;
  }

  //
  // Some legacy GOPs leave PixelsPerScanLine zero even for otherwise valid
  // modes. Rejecting those modes leaves the console with no usable mode at
  // all, so fall back to the horizontal resolution as the stride.
  //
  ScanLine = Info->PixelsPerScanLine != 0 ? Info->PixelsPerScanLine : Info->HorizontalResolution;

  if (BaseOverflowTriMulUN (
        ScanLine,
        Info->VerticalResolution,
        BytesPerPixel,
        &Size
        ))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Size == 0) {
    return EFI_NOT_FOUND;
  }

  *FrameBufferSize = Size;

  return EFI_SUCCESS;
}

//
// Use PAT to enable write-combining caching (burst mode) on GOP memory,
// when it is suppported but firmware has not set it up.
//
STATIC
EFI_STATUS
WriteCombineGop (
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop,
  BOOLEAN                       SetPatWC
  )
{
  EFI_STATUS                      Status;
  MTRR_MEMORY_CACHE_TYPE          MtrrCacheType;
  PAGE_MAP_AND_DIRECTORY_POINTER  *PageTable;
  EFI_VIRTUAL_ADDRESS             VirtualAddress;
  EFI_PHYSICAL_ADDRESS            PhysicalAddress;
  UINTN                           FrameBufferSize;
  UINT64                          Bits;
  UINT8                           Level;
  BOOLEAN                         HasMtrr;
  BOOLEAN                         HasPat;
  UINT64                          PatMsr;
  UINT64                          ModifiedPatMsr;
  PAT_INDEX                       PatIndex;
  BOOLEAN                         HasDefaultPat;
  BOOLEAN                         AlreadySet;

  if (!SetPatWC) {
    return EFI_SUCCESS;
  }

  if (  (Gop == NULL)
     || (Gop->Mode == NULL)
     || (Gop->Mode->Info == NULL))
  {
    ASSERT (
      (Gop != NULL)
           && (Gop->Mode != NULL)
           && (Gop->Mode->Info != NULL)
      );
    return EFI_UNSUPPORTED;
  }

  Status = OcGopModeSafeFrameBufferSize (Gop->Mode, &FrameBufferSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  HasMtrr = IsMtrrSupported ();
  HasPat  = OcIsPatSupported ();

  DEBUG ((
    DEBUG_INFO,
    "OCC: MTRR %asupported, PAT %asupported\n",
    HasMtrr ? "" : "not ",
    HasPat ? "" : "not "
    ));

  if (!HasMtrr || !HasPat) {
    return EFI_UNSUPPORTED;
  }

  AlreadySet = FALSE;
  PageTable  = OcGetCurrentPageTable (NULL);

  do {
    PatMsr        = AsmReadMsr64 (MSR_IA32_CR_PAT);
    HasDefaultPat = (PatMsr == PAT_DEFAULTS);

    DEBUG ((
      DEBUG_INFO,
      "OCC: PAT 0x%016LX (%a)\n",
      PatMsr,
      HasDefaultPat ? "default" : "not default"
      ));

    MtrrCacheType = MtrrGetMemoryAttribute (Gop->Mode->FrameBufferBase);

    VirtualAddress = Gop->Mode->FrameBufferBase;

    Status = OcGetSetPageTableInfoForAddress (
               PageTable,
               VirtualAddress,
               &PhysicalAddress,
               &Level,
               &Bits,
               &PatIndex,
               FALSE
               );

    DEBUG ((
      DEBUG_INFO,
      "OCC: 0x%LX MTRR %u=%a PTE%u bits 0x%016LX PAT@%u->%u=%a - %r\n",
      VirtualAddress,
      MtrrCacheType,
      OcGetMtrrTypeString (MtrrCacheType),
      Level,
      Bits,
      PatIndex.Index,
      GET_PAT_N (PatMsr, PatIndex.Index),
      OcGetPatTypeString (GET_PAT_N (PatMsr, PatIndex.Index)),
      Status
      ));

    if (EFI_ERROR (Status)) {
      ASSERT (!AlreadySet);
      return Status;
    }

    ASSERT (VirtualAddress == PhysicalAddress);

    if (AlreadySet) {
      break;
    }

    //
    // Attempting to set again if set in PAT works on some systems (including if set
    // before by us) but fails with a hang on some others, so avoid it even though we
    // might otherwise prefer to make sure to set the whole memory for the current mode.
    // Definitely no need to set again if set in MTRR.
    //
    if (  (MtrrCacheType == CacheWriteCombining)
       || (GET_PAT_N (PatMsr, PatIndex.Index) == PatWriteCombining)
          )
    {
      return EFI_ALREADY_STARTED;
    }

    //
    // We cannot split MTRR ranges (returns out of resources, meaning there are not
    // enough MTRRs to keep the existing mapping and insert a new one), so set up
    // PAT7 as write combining (WC) and modify the page tables instead.
    //
    if (HasDefaultPat) {
      //
      // Modify PAT MSR to add WC entry somewhere reasonable (not first four entries,
      // and should use an entry likely to be unused) in PAT MSR.
      // TODO: Could fully check existing page tables to confirm PAT7 is unused.
      //
      ModifiedPatMsr = MODIFY_PAT_MSR (PatMsr, PAT_INDEX_TO_CHANGE, PatWriteCombining);
      DEBUG ((DEBUG_INFO, "OCC: Writing PAT 0x%016LX\n", ModifiedPatMsr));
      AsmWriteMsr64 (MSR_IA32_CR_PAT, ModifiedPatMsr);

      PatIndex.Index = PAT_INDEX_TO_CHANGE;
    } else {
      //
      // Do not modify non-default PAT MSR, but use WC if it already exists there
      // (including previously set by us, if GOP mode changes more than once).
      //
      for (PatIndex.Index = 0; PatIndex.Index < PAT_INDEX_MAX; PatIndex.Index++) {
        if (GET_PAT_N (PatMsr, PatIndex.Index) == PatWriteCombining) {
          break;
        }
      }

      if (PatIndex.Index == PAT_INDEX_MAX) {
        Status = EFI_UNSUPPORTED;
        DEBUG ((DEBUG_INFO, "OCC: Non-default PAT MSR with no WC entry - %r\n", Status));
        return Status;
      }
    }

    //
    // TODO: Use full GOP memory range not just range in use for current mode?
    //
    Status = OcSetPatIndexForAddressRange (
               PageTable,
               Gop->Mode->FrameBufferBase,
               FrameBufferSize,
               &PatIndex
               );

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OCC: Failed to set PAT index for range - %r\n", Status));
      return Status;
    }

    DEBUG_CODE_BEGIN ();
    AlreadySet = TRUE;
    DEBUG_CODE_END ();
  } while (AlreadySet);

  return EFI_SUCCESS;
}

EFI_STATUS
OcSetGopBurstMode (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Status = gBS->HandleProtocol (
                  gST->ConsoleOutHandle,
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID **)&Gop
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OCC: No console GOP found for burst mode - %r\n", Status));
    return Status;
  }

  Status = WriteCombineGop (Gop, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      (Status == EFI_ALREADY_STARTED) ? DEBUG_INFO : DEBUG_WARN,
      "OCC: Failed to set burst mode - %r\n",
      Status
      ));
  }

  return Status;
}

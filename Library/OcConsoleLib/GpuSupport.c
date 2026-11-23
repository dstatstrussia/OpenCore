/** @file
  Support tables and helpers for the older display adapters this build
  supports: Intel HD3000 (Sandy Bridge) and NVIDIA GT 520MX (Fermi), both in
  desktop and mobile variants.

  Copyright (c) 2026, OpenCode. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD
  License which accompanies this distribution.  The full text of the license
  may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "OcConsoleLibInternal.h"

#include <Library/BaseLib.h>

//
// Intel HD3000 (Sandy Bridge) device IDs: desktop GT1 0x0102, GT2 0x0112,
// mobile GT1 0x0106, mobile GT2 0x0116, plus the remaining Apple family
// members 0x010A, 0x0122 and 0x0126.
//
STATIC CONST UINT16  mHd3000DeviceIds[] = {
  0x0102, 0x0106, 0x010A, 0x0112, 0x0116, 0x0122, 0x0126
};

//
// NVIDIA GT 520M/520MX (Fermi) device IDs.
//
STATIC CONST UINT16  mGt520MxDeviceIds[] = {
  0x1050, 0x1054, 0x1057
};

//
// Vendor IDs.
//
#define OC_GPU_VENDOR_INTEL   0x8086
#define OC_GPU_VENDOR_NVIDIA  0x10DE

STATIC
BOOLEAN
IsKnownDeviceId (
  IN CONST UINT16  *DeviceIds,
  IN UINTN         Count,
  IN UINT16        DeviceId
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; Index++) {
    if (DeviceIds[Index] == DeviceId) {
      return TRUE;
    }
  }

  return FALSE;
}

BOOLEAN
OcGpuIsHd3000 (
  IN UINT16  VendorId,
  IN UINT16  DeviceId
  )
{
  return VendorId == OC_GPU_VENDOR_INTEL
         && IsKnownDeviceId (
              mHd3000DeviceIds,
              sizeof (mHd3000DeviceIds) / sizeof (mHd3000DeviceIds[0]),
              DeviceId
              );
}

BOOLEAN
OcGpuIsGt520Mx (
  IN UINT16  VendorId,
  IN UINT16  DeviceId
  )
{
  return VendorId == OC_GPU_VENDOR_NVIDIA
         && IsKnownDeviceId (
              mGt520MxDeviceIds,
              sizeof (mGt520MxDeviceIds) / sizeof (mGt520MxDeviceIds[0]),
              DeviceId
              );
}

OC_GPU_KIND
OcGpuKindOf (
  IN UINT16  VendorId,
  IN UINT16  DeviceId
  )
{
  if (OcGpuIsHd3000 (VendorId, DeviceId)) {
    return OcGpuKindHd3000;
  }

  if (OcGpuIsGt520Mx (VendorId, DeviceId)) {
    return OcGpuKindGt520Mx;
  }

  return OcGpuKindUnknown;
}

/** @file
  Copyright (C) 2026. All rights reserved.

  Dynamic Binary Translation DXE Driver for ARM64 to x86_64
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/OcDbtLib.h>

EFI_STATUS
EFIAPI
OpenDbvX64EntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS     Status;
  DBT_CONTEXT    *DbtContext;

  Status = DbtInitContext (&DbtContext, 0x10000);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DBT ARM64->x86_64 initialized\n"));

  return EFI_SUCCESS;
}
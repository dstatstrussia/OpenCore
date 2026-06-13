/** @file
    Copyright (C) 2026. All rights reserved.

    Dynamic Binary Translation DXE Driver for ARM64 to x86_64
**/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/OcDbtLib.h>
#include <Library/OcBootManagementLib.h>

#include <Protocol/OcBootEntry.h>

STATIC DBT_CONTEXT  *gDbtContext = NULL;

STATIC
EFI_STATUS
EFIAPI
OcGetDbtBootEntries (
  IN OUT         OC_PICKER_CONTEXT  *PickerContext,
  IN     CONST EFI_HANDLE           Device OPTIONAL,
  OUT       OC_PICKER_ENTRY         **Entries,
  OUT       UINTN                   *NumEntries
  )
{
  EFI_STATUS  Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *RootDirectory;
  EFI_FILE_PROTOCOL                *BootDirectory;
  EFI_FILE_INFO                    *FileInfo;
  UINTN                            FileInfoSize;
  UINTN                            EntryCount;
  OC_PICKER_ENTRY                  *NewEntries;
  BOOLEAN                          IsMacSoftwareUpdate = FALSE;

  ASSERT (PickerContext != NULL);
  ASSERT (Entries != NULL);
  ASSERT (NumEntries != NULL);

  *Entries    = NULL;
  *NumEntries = 0;

  if (Device == NULL) {
    DEBUG ((DEBUG_INFO, "DBT: Device is NULL, returning EFI_NOT_FOUND\n"));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "DBT: GetBootEntries called for Device %p\n", Device));

  Status = gBS->HandleProtocol (
                   Device,
                   &gEfiSimpleFileSystemProtocolGuid,
                   (VOID **)&FileSystem
                   );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "DBT: HandleProtocol failed - %r\n", Status));
    return Status;
  }

  Status = FileSystem->OpenVolume (FileSystem, &RootDirectory);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "DBT: OpenVolume failed - %r\n", Status));
    return Status;
  }

  EntryCount = 0;

  //
  // Look for macOS Installer (com.apple.installer) in boot directories
  //
  DEBUG ((DEBUG_INFO, "DBT: Looking for traditional installer at %s\n", L"\\System\\Library\\CoreServices\\com.apple.installer"));
  Status = RootDirectory->Open (
                           RootDirectory,
                           &BootDirectory,
                           L"\\System\\Library\\CoreServices\\com.apple.installer",
                           EFI_FILE_MODE_READ,
                           0
                           );

  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "DBT: Found traditional installer directory\n"));
    Status = EFI_NOT_FOUND;

    FileInfoSize = 0;
    BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, NULL);
    if (FileInfoSize > 0) {
      Status = EFI_SUCCESS;
      FileInfo = AllocatePool (FileInfoSize);
      if (FileInfo != NULL) {
        BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
        if ((FileInfo->Attribute & EFI_FILE_DIRECTORY) != 0) {
          DEBUG ((DEBUG_INFO, "DBT: Traditional installer is a directory, EntryCount++\n"));
          ++EntryCount;
        } else {
          DEBUG ((DEBUG_INFO, "DBT: Traditional installer is NOT a directory (attributes: 0x%x)\n", FileInfo->Attribute));
        }
        FreePool (FileInfo);
      }
    }
    BootDirectory->Close (BootDirectory);
  } else {
    DEBUG ((DEBUG_INFO, "DBT: Traditional installer not found - %r\n", Status));
  }

  //
  // Also look for macOS 27+ installer (com.apple.MobileAsset) in SharedSupport
  //
  if (EntryCount == 0) {
    DEBUG ((DEBUG_INFO, "DBT: Looking for macOS 27+ MobileAsset installer at %s\n", L"\\SharedSupport\\com_apple_MobileAsset_MacSoftwareUpdate"));
    Status = RootDirectory->Open (
                             RootDirectory,
                             &BootDirectory,
                             L"\\SharedSupport\\com_apple_MobileAsset_MacSoftwareUpdate",
                             EFI_FILE_MODE_READ,
                             0
                             );

    if (!EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "DBT: Found macOS 27+ MobileAsset installer directory, IsMacSoftwareUpdate = TRUE\n"));
      Status = EFI_NOT_FOUND;

      FileInfoSize = 0;
      BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, NULL);
      if (FileInfoSize > 0) {
        Status = EFI_SUCCESS;
        FileInfo = AllocatePool (FileInfoSize);
        if (FileInfo != NULL) {
          BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
          if ((FileInfo->Attribute & EFI_FILE_DIRECTORY) != 0) {
            DEBUG ((DEBUG_INFO, "DBT: MobileAsset installer is a directory, EntryCount++, IsMacSoftwareUpdate=TRUE\n"));
            ++EntryCount;
            IsMacSoftwareUpdate = TRUE;
          } else {
            DEBUG ((DEBUG_INFO, "DBT: MobileAsset installer is NOT a directory (attributes: 0x%x)\n", FileInfo->Attribute));
          }
          FreePool (FileInfo);
        }
      }
      BootDirectory->Close (BootDirectory);
    } else {
      DEBUG ((DEBUG_INFO, "DBT: macOS 27+ MobileAsset installer not found - %r\n", Status));
    }
  }

  DEBUG ((DEBUG_INFO, "DBT: Installer scan complete - EntryCount=%u, IsMacSoftwareUpdate=%d\n", EntryCount, IsMacSoftwareUpdate));
  RootDirectory->Close (RootDirectory);

  if (EntryCount > 0) {
    DEBUG ((DEBUG_INFO, "DBT: Creating %u installer entry(s)\n", EntryCount));
    NewEntries = AllocatePool (sizeof (OC_PICKER_ENTRY) * EntryCount);
    if (NewEntries == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    ZeroMem (NewEntries, sizeof (OC_PICKER_ENTRY) * EntryCount);

    NewEntries[0].Id = AllocateCopyPool (AsciiStrSize ("macOS-Installer"), "macOS-Installer");
    NewEntries[0].Name = AllocateCopyPool (AsciiStrSize ("macOS Installer (Translated)"), "macOS Installer (Translated)");
    NewEntries[0].Flavour = AllocateCopyPool (AsciiStrSize ("OpenDbt"), "OpenDbt");
    //
    // For macOS 27+ MobileAsset installer, use alternate boot path
    //
    if (IsMacSoftwareUpdate) {
      DEBUG ((DEBUG_INFO, "DBT: Using macOS 27+ MobileAsset boot path: %s\n", "\\SharedSupport\\boot.efi"));
      NewEntries[0].Path = AllocateCopyPool (AsciiStrSize ("\\SharedSupport\\boot.efi"), "\\SharedSupport\\boot.efi");
    } else {
      DEBUG ((DEBUG_INFO, "DBT: Using traditional installer boot path: %s\n", "\\System\\Library\\CoreServices\\boot.efi"));
      NewEntries[0].Path = AllocateCopyPool (AsciiStrSize ("\\System\\Library\\CoreServices\\boot.efi"), "\\System\\Library\\CoreServices\\boot.efi");
    }

    *Entries    = NewEntries;
    *NumEntries = EntryCount;
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_INFO, "DBT: No installer found, returning EFI_NOT_FOUND\n"));
  return EFI_NOT_FOUND;
}

STATIC
VOID
EFIAPI
OcFreeDbtBootEntries (
  IN  OC_PICKER_ENTRY  **Entries,
  IN  UINTN            NumEntries
  )
{
  UINTN  Index;

  if ((Entries == NULL) || (*Entries == NULL)) {
    return;
  }

  for (Index = 0; Index < NumEntries; Index++) {
    if ((*Entries)[Index].Id != NULL) {
      FreePool ((VOID *)(UINTN)(*Entries)[Index].Id);
    }
    if ((*Entries)[Index].Name != NULL) {
      FreePool ((VOID *)(UINTN)(*Entries)[Index].Name);
    }
    if ((*Entries)[Index].Flavour != NULL) {
      FreePool ((VOID *)(UINTN)(*Entries)[Index].Flavour);
    }
    if ((*Entries)[Index].Path != NULL) {
      FreePool ((VOID *)(UINTN)(*Entries)[Index].Path);
    }
  }

  FreePool (*Entries);
  *Entries = NULL;
}

STATIC OC_BOOT_ENTRY_PROTOCOL  mDbtBootEntryProtocol = {
  OC_BOOT_ENTRY_PROTOCOL_REVISION,
  OcGetDbtBootEntries,
  OcFreeDbtBootEntries,
  NULL
};

EFI_STATUS
EFIAPI
OpenDbvX64EntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = DbtInitContext (&gDbtContext, 0x10000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DBT ARM64->x86_64 initialized\n"));

  //
  // Install boot entry protocol to provide installer entries
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                   &ImageHandle,
                   &gOcBootEntryProtocolGuid,
                   &mDbtBootEntryProtocol,
                   NULL
                   );

  if (EFI_ERROR (Status)) {
    DbtFreeContext (gDbtContext);
    gDbtContext = NULL;
    return Status;
  }

  return EFI_SUCCESS;
}
/** @file
     Copyright (C) 2026. All rights reserved.

     Dynamic Binary Translation DXE Driver for ARM64 to x86_64
  **/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Guid/AppleApfsInfo.h>
#include <IndustryStandard/AppleBootArgs.h>
#include <IndustryStandard/AppleFatBinaryImage.h>
#include <IndustryStandard/AppleMachoImage.h>

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
#include <Library/OcAppleKernelLib.h>
#include <Library/OcMachoLib.h>

#include <Protocol/OcBootEntry.h>
#include <Protocol/SimpleFileSystem.h>

STATIC DBT_CONTEXT  *gDbtContext = NULL;
STATIC EFI_HANDLE   gInstallerDevice = NULL;

STATIC
BOOLEAN
IsGoldenGateInstaller (
  IN  EFI_FILE_PROTOCOL  *RootDirectory
  )
{
  EFI_STATUS  Status;
  EFI_FILE_PROTOCOL  *File;
  CHAR16  *MarkerPath = L"\\.IAPhysicalMedia";

  Status = RootDirectory->Open (
                          RootDirectory,
                          &File,
                          MarkerPath,
                          EFI_FILE_MODE_READ,
                          0
                          );

  if (!EFI_ERROR (Status)) {
    File->Close (File);
    return TRUE;
  }

  return FALSE;
}

STATIC
BOOLEAN
IsSharedSupportVolume (
  IN  EFI_FILE_PROTOCOL  *RootDirectory
  )
{
  EFI_STATUS  Status;
  EFI_FILE_PROTOCOL  *Dir;
  CHAR16  *Path = L"\\com_apple_MobileAsset_MacSoftwareUpdate";

  Status = RootDirectory->Open (
                           RootDirectory,
                           &Dir,
                           Path,
                           EFI_FILE_MODE_READ,
                           0
                           );

  if (!EFI_ERROR (Status)) {
    Dir->Close (Dir);
    return TRUE;
  }

  return FALSE;
}

STATIC
BOOLEAN
IsPrebootVolume (
  IN  EFI_HANDLE  Device
  )
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_FILE_PROTOCOL               *RootDirectory;
  APPLE_APFS_VOLUME_INFO          *VolumeInfo;

  Status = gBS->HandleProtocol (
                  Device,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Status = FileSystem->OpenVolume (FileSystem, &RootDirectory);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  VolumeInfo = OcGetFileInfo (
                  RootDirectory,
                  &gAppleApfsVolumeInfoGuid,
                  sizeof (*VolumeInfo),
                  NULL
                  );

  RootDirectory->Close (RootDirectory);

  if (VolumeInfo == NULL) {
    return FALSE;
  }

  if ((VolumeInfo->Role & APPLE_APFS_VOLUME_ROLE_PREBOOT) != 0) {
    DEBUG ((DEBUG_INFO, "DBT: Device is APFS Preboot volume\n"));
    FreePool (VolumeInfo);
    return TRUE;
  }

  FreePool (VolumeInfo);
  return FALSE;
}

STATIC
BOOLEAN
IsArm64Kernel (
  IN  UINT8    *KernelBuffer,
  IN  UINT32    KernelSize,
  IN  BOOLEAN   Prefer32Bit
  )
{
  UINT32  Magic;
  INT32   CpuType;
  UINT32  Offset;
  UINT32  Size;

  if (KernelSize < sizeof (UINT32)) {
    return FALSE;
  }

  Magic = *((UINT32 *)KernelBuffer);

  if (Magic == MACH_HEADER_64_SIGNATURE) {
    MACH_HEADER_64  *Header64 = (MACH_HEADER_64 *)KernelBuffer;
    CpuType = Header64->cputype;
    return CpuType == MachCpuTypeArm64 || CpuType == MachCpuTypeArm6432;
  } else if (Magic == MACH_FAT_BINARY_SIGNATURE || Magic == MACH_FAT_BINARY_INVERT_SIGNATURE) {
    EFI_STATUS  Status;
    Status = FatGetArchitectureOffset (
               KernelBuffer,
               sizeof (MACH_HEADER_64),
               KernelSize,
               !Prefer32Bit ? MachCpuTypeArm64 : MachCpuTypeX8664,
               &Offset,
               &Size
               );
    if (!EFI_ERROR (Status)) {
      return !Prefer32Bit;
    }

    Status = FatGetArchitectureOffset (
               KernelBuffer,
               sizeof (MACH_HEADER_64),
               KernelSize,
               Prefer32Bit ? MachCpuTypeArm64 : MachCpuTypeX8664,
               &Offset,
               &Size
               );
    if (!EFI_ERROR (Status)) {
      return Prefer32Bit;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
DirectLoadKernel (
  IN  OC_PICKER_CONTEXT  *PickerContext,
  IN  EFI_HANDLE         Device,
  IN  CONST CHAR16       *KernelPath
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *RootDirectory;
  EFI_FILE_PROTOCOL                *KernelFile;
  UINT8                            *KernelBuffer;
  UINT32                           KernelSize;
  UINT32                           AllocatedSize;
  BOOLEAN                          Is32Bit;
  OC_MACHO_CONTEXT                 MachoContext;
  UINT64                           EntryPoint;
  BootArgs2                       *BootArgs;
  UINTN                            BootArgsSize;
  BOOLEAN                          IsArm64;

  Status = gBS->HandleProtocol (
                  Device,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FileSystem->OpenVolume (FileSystem, &RootDirectory);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  KernelBuffer = NULL;
  KernelSize = 0;
  AllocatedSize = 0;

  Status = ReadAppleKernel (
            KernelFile,
            FALSE,  // Prefer 64-bit
            &Is32Bit,
            &KernelBuffer,
            &KernelSize,
            &AllocatedSize,
            0,    // Reserved size
            NULL  // No digest
            );

  KernelFile->Close (KernelFile);
  RootDirectory->Close (RootDirectory);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DirectKernel: Failed to read kernel - %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DirectKernel: Loaded kernel %u bytes (%a)\n",
          KernelSize, Is32Bit ? "32-bit" : "64-bit"));

  ZeroMem (&MachoContext, sizeof (MachoContext));
  IsArm64 = IsArm64Kernel (KernelBuffer, KernelSize, FALSE);

  if (!MachoInitializeContext (
         &MachoContext,
         KernelBuffer,
         KernelSize,
         0,
         KernelSize,
         Is32Bit
         )) {
    DEBUG ((DEBUG_ERROR, "DirectKernel: Failed to initialize Mach-O context\n"));
    FreePool (KernelBuffer);
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get entry point from LC_UNIXTHREAD
  // Note: MachoRuntimeGetEntryAddress only handles x86 thread states.
  // For ARM64 kernels, we need platform-specific handling.
  //
  if (!IsArm64) {
    EntryPoint = MachoRuntimeGetEntryAddress (KernelBuffer);
    if (EntryPoint == 0) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: Failed to get entry point from Mach-O\n"));
      FreePool (KernelBuffer);
      return EFI_INVALID_PARAMETER;
    }
  } else {
    //
    // For ARM64 kernels, extract entry point from thread state
    // ARM64 thread state (arm_thread_state64_t) has PC at offset 0x108
    //
    MACH_HEADER_64  *Header64 = (MACH_HEADER_64 *)KernelBuffer;
    MACH_LOAD_COMMAND  *Cmd;
    UINTN  Index;
    UINTN  TopOfCommands;

    EntryPoint = 0;
    TopOfCommands = (UINTN)&Header64->Commands[0] + Header64->CommandsSize;

    for (Index = 0; Index < Header64->NumCommands; ++Index) {
      Cmd = &Header64->Commands[Index];
      if (Cmd->CommandType == MACH_LOAD_COMMAND_UNIX_THREAD) {
        UINT64  *ThreadState = (UINT64 *)((UINTN)Cmd + sizeof (MACH_THREAD_COMMAND));
        if ((UINTN)&ThreadState[6] <= TopOfCommands) {
          EntryPoint = ThreadState[6];
        }
        break;
      }
    }

    if (EntryPoint == 0) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: Failed to get entry point from ARM64 Mach-O\n"));
      FreePool (KernelBuffer);
      return EFI_INVALID_PARAMETER;
    }
  }

  DEBUG ((DEBUG_INFO, "DirectKernel: Entry point at 0x%llx\n", EntryPoint));

  BootArgsSize = sizeof (BootArgs2);
  BootArgs = AllocatePool (BootArgsSize);
  if (BootArgs == NULL) {
    FreePool (KernelBuffer);
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (BootArgs, BootArgsSize);
  BootArgs->Revision = kBootArgsRevision2_0;
  BootArgs->Version  = kBootArgsVersion2;
  BootArgs->efiMode  = Is32Bit ? kBootArgsEfiMode32 : kBootArgsEfiMode64;
  AsciiSPrint (BootArgs->CommandLine, BOOT_LINE_LENGTH, "install=1");

  BootArgs->kaddr = (UINT64)(UINTN)KernelBuffer;
  BootArgs->ksize  = KernelSize;

  BootArgs->deviceTreeP = 0;
  BootArgs->deviceTreeLength = 0;

  //
  // For ARM64 kernel, use DBT translation
  //
  if (IsArm64 && gDbtContext != NULL) {
    DBT_ARM64_CONTEXT  ArmContext;

    ZeroMem (&ArmContext, sizeof (ArmContext));
    ArmContext.X0 = (UINT64)(UINTN)BootArgs;  // First argument: boot_args
    ArmContext.SP = 0;                       // Stack pointer (must be set up)
    ArmContext.PC = EntryPoint;              // Entry point (already virtual address)

    DEBUG ((DEBUG_INFO, "DirectKernel: Executing ARM64 kernel via DBT\n"));

    Status = DbtTranslateBlock (gDbtContext, KernelBuffer, KernelSize, NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: DBT translation failed - %r\n", Status));
      FreePool (BootArgs);
      FreePool (KernelBuffer);
      return Status;
    }

    DbtExecute (gDbtContext, &ArmContext);

    // Should not reach here
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_DEVICE_ERROR;
  } else if (!IsArm64) {
    //
    // x86_64 kernel - directly call entry point
    //
    DEBUG ((DEBUG_INFO, "DirectKernel: x86_64 kernel execution not fully implemented\n"));
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_UNSUPPORTED;
  } else {
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_UNSUPPORTED;
  }
}

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
  EFI_FILE_PROTOCOL                *FileIterator;
  EFI_FILE_INFO                    *FileInfo;
  UINTN                            FileInfoSize;
  UINTN                            EntryCount;
  OC_PICKER_ENTRY                  *NewEntries;
  UINTN                            BufferSize;
  EFI_STATUS                       TmpStatus;
  CHAR16                           *SubPath;
  UINTN                            SubPathSize;
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

  //
  // Also look for macOS 27+ installer dyld cache path (x86_64 cache in installer)
  //
  if (EntryCount == 0) {
    DEBUG ((DEBUG_INFO, "DBT: Looking for macOS 27+ dyld cache installer at %s\n", L"\\System\\Library\\dyld"));
    Status = RootDirectory->Open (
                             RootDirectory,
                             &BootDirectory,
                             L"\\System\\Library\\dyld",
                             EFI_FILE_MODE_READ,
                             0
                             );

    if (!EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "DBT: Found dyld cache directory, checking for x86_64 cache\n"));

      FileInfoSize = 0;
      BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, NULL);
      if (FileInfoSize > 0) {
        FileInfo = AllocatePool (FileInfoSize);
        if (FileInfo != NULL) {
          BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
          if ((FileInfo->Attribute & EFI_FILE_DIRECTORY) != 0) {
            EFI_FILE_PROTOCOL *DylibDir;
            Status = BootDirectory->Open (
                                     BootDirectory,
                                     &DylibDir,
                                     L"shared_cache.x86_64h",
                                     EFI_FILE_MODE_READ,
                                     0
                                     );
            if (EFI_ERROR (Status)) {
              Status = BootDirectory->Open (
                                       BootDirectory,
                                       &DylibDir,
                                       L"shared_cache.x86_64",
                                       EFI_FILE_MODE_READ,
                                       0
                                       );
            }
            if (!EFI_ERROR (Status)) {
              DEBUG ((DEBUG_INFO, "DBT: Found x86_64 dyld shared cache, EntryCount++\n"));
              ++EntryCount;
              DylibDir->Close (DylibDir);
            }
          }
          FreePool (FileInfo);
        }
      }
      BootDirectory->Close (BootDirectory);
    }
  }

  //
  // Also look for macOS 27+ installer kernel in SharedSupport (DirectKernel bypass)
  //
  if (EntryCount == 0) {
    STATIC CONST CHAR16  *KernelPaths[] = {
      L"\\SharedSupport\\kernel",
      L"\\System\\Library\\Kernels\\kernel",
      NULL
    };

    for (UINTN Index = 0; KernelPaths[Index] != NULL; Index++) {
      DEBUG ((DEBUG_INFO, "DBT: Looking for DirectKernel at %s\n", KernelPaths[Index]));
      Status = RootDirectory->Open (
                              RootDirectory,
                              &BootDirectory,
                              KernelPaths[Index],
                              EFI_FILE_MODE_READ,
                              0
                              );

      if (!EFI_ERROR (Status)) {
        FileInfoSize = 0;
        BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, NULL);
        if (FileInfoSize > 0) {
          FileInfo = AllocatePool (FileInfoSize);
          if (FileInfo != NULL) {
            BootDirectory->GetInfo (BootDirectory, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
            if ((FileInfo->Attribute & EFI_FILE_DIRECTORY) == 0) {
              DEBUG ((DEBUG_INFO, "DBT: Found installer kernel at %s, EntryCount++\n", KernelPaths[Index]));
              ++EntryCount;
              IsMacSoftwareUpdate = TRUE;
            }
            FreePool (FileInfo);
          }
        }
        BootDirectory->Close (BootDirectory);
        if (EntryCount > 0) {
          break;
        }
      }
    }
  }

  DEBUG ((DEBUG_INFO, "DBT: Installer scan complete - EntryCount=%u, IsMacSoftwareUpdate=%d\n", EntryCount, IsMacSoftwareUpdate));

  if (EntryCount == 0) {
    //
    // macOS 27 Golden Gate: Check for .IAPhysicalMedia marker
    //
    if (IsGoldenGateInstaller (RootDirectory)) {
      DEBUG ((DEBUG_INFO, "DBT: Found .IAPhysicalMedia marker - macOS 27 Golden Gate installer detected\n"));
      DEBUG ((DEBUG_INFO, "DBT: Checking for mounted SharedSupport volume...\n"));

      gInstallerDevice = Device;

      EFI_HANDLE  *HandleBuffer;
      UINTN       HandleCount;
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FsProtocol;
      EFI_FILE_PROTOCOL                *SharedRoot;

      Status = gBS->LocateHandleBuffer (
                      ByProtocol,
                      &gEfiSimpleFileSystemProtocolGuid,
                      NULL,
                      &HandleCount,
                      &HandleBuffer
                      );

      if (!EFI_ERROR (Status) && HandleCount > 0) {
        for (UINTN Idx = 0; Idx < HandleCount; Idx++) {
          if (HandleBuffer[Idx] == Device) {
            continue;
          }

          Status = gBS->HandleProtocol (
                       HandleBuffer[Idx],
                       &gEfiSimpleFileSystemProtocolGuid,
                       (VOID **)&FsProtocol
                       );

          if (!EFI_ERROR (Status)) {
            Status = FsProtocol->OpenVolume (FsProtocol, &SharedRoot);
            if (!EFI_ERROR (Status)) {
              if (IsSharedSupportVolume (SharedRoot)) {
                DEBUG ((DEBUG_INFO, "DBT: Found mounted SharedSupport volume for Golden Gate installer\n"));
                EntryCount = 1;
                IsMacSoftwareUpdate = TRUE;
                SharedRoot->Close (SharedRoot);
                break;
              }
              SharedRoot->Close (SharedRoot);
            }
          }
        }
      }

      if (HandleBuffer != NULL) {
        FreePool (HandleBuffer);
      }
    }
  }

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
    //
    // For macOS 27+ DirectKernel installer, use DirectKernel flavour
    //
    if (IsMacSoftwareUpdate && gDbtContext != NULL) {
      NewEntries[0].Flavour = AllocateCopyPool (AsciiStrSize ("DirectKernel"), "DirectKernel");
      NewEntries[0].Path = AllocateCopyPool (AsciiStrSize ("\\SharedSupport\\kernel"), "\\SharedSupport\\kernel");
      NewEntries[0].UnmanagedBootAction = (OC_BOOT_UNMANAGED_ACTION)DirectLoadKernel;
      NewEntries[0].UnmanagedBootGetFinalDevicePath = NULL;
    } else {
      NewEntries[0].Flavour = AllocateCopyPool (AsciiStrSize ("OpenDbt"), "OpenDbt");
      if (IsMacSoftwareUpdate) {
        DEBUG ((DEBUG_INFO, "DBT: Using macOS 27+ MobileAsset boot path: %s\n", "\\SharedSupport\\boot.efi"));
        NewEntries[0].Path = AllocateCopyPool (AsciiStrSize ("\\SharedSupport\\boot.efi"), "\\SharedSupport\\boot.efi");
      } else {
        DEBUG ((DEBUG_INFO, "DBT: Using traditional installer boot path: %s\n", "\\System\\Library\\CoreServices\\boot.efi"));
        NewEntries[0].Path = AllocateCopyPool (AsciiStrSize ("\\System\\Library\\CoreServices\\boot.efi"), "\\System\\Library\\CoreServices\\boot.efi");
      }
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

  Status = DbtInitContext (&gDbtContext, 0x100000);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DBT: Failed to initialize DBT context - %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DBT: ARM64->x86_64 initialized for DirectKernel\n"));

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
/** @file
      Copyright (C) 2026. All rights reserved.

      Dynamic Binary Translation DXE Driver for ARM64 to x86_64
   **/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Guid/AppleApfsInfo.h>
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
#include <Library/OcMemoryLib.h>
#include <Library/OcDeviceTreeLib.h>

#include <Library/OcCompressionLib/zlib/zlib.h>

#include <Protocol/OcBootEntry.h>
#include <Protocol/OcLog.h>
#include <Protocol/SimpleFileSystem.h>

//
// ARM64 thread state flavor for Mach-O LC_UNIXTHREAD
//
#define ARM64_THREAD_STATE_FLAVOR  6

//
// ZIP format constants
//
#define ZIP_LOCAL_FILE_SIGNATURE   0x04034B50U
#define ZIP_EOCD_SIGNATURE         0x06054B50U
#define ZIP_METHOD_STORED          0
#define ZIP_METHOD_DEFLATED        8

STATIC DBT_CONTEXT  *gDbtContext     = NULL;
STATIC EFI_HANDLE   gInstallerDevice = NULL;
STATIC EFI_HANDLE   gX86ImageHandle  = NULL;

//
// Force the OpenCore log protocol to maximum verbosity on both the console
// and in the log file.  Called from the entry point (covers the picker),
// from the boot entry enumeration (covers the boot picker scan) and from
// the boot entry action (covers the DirectKernel / macOS Translated boot).
//
STATIC
VOID
RaiseScreenVerbose (
  VOID
  )
{
  OC_LOG_PROTOCOL  *OcLog;
  EFI_STATUS       Status;

  Status = gBS->LocateProtocol (&gOcLogProtocolGuid, NULL, (VOID **)&OcLog);
  if (EFI_ERROR (Status) || OcLog == NULL) {
    DEBUG ((DEBUG_WARN, "DBT: OcLog protocol not found, screen trace unavailable\n"));
    return;
  }

  OcLog->DisplayLevel = (UINTN)0xFFFFFFFF;
  OcLog->Options     |= OC_LOG_CONSOLE | OC_LOG_FILE;
  OcLog->DisplayDelay = 0;
  DEBUG ((DEBUG_INFO, "DBT: on-screen log level raised to 0xFFFFFFFF\n"));
}

//
// True when an ARM64 instruction updates PC (so a translated block must end
// there for the dispatcher to continue from the new address).
//
STATIC
BOOLEAN
IsPcUpdatingBranch (
  IN UINT32  Inst
  )
{
  if ((Inst & 0x7C000000) == 0x14000000) return TRUE;   // B / BL
  if ((Inst & 0xFF000010) == 0x54000000) return TRUE;   // B.cond
  if ((Inst & 0x7E000000) == 0x34000000) return TRUE;   // CBZ / CBNZ
  if ((Inst & 0x7E000000) == 0x36000000) return TRUE;   // TBZ / TBNZ
  if ((Inst & 0xFE000000) == 0xD6000000) return TRUE;   // BR / BLR / RET
  return FALSE;
}

//
// Allocate the kernel image buffer below 1 GB if possible.  AllocatePool
// usually lands in the 2.3-2.6 GB region where reads are unreliable on
// this platform (observed: a UINT32 read of 0x01CEE165 and a UINT8 read
// of 0x00 at the very same address), which corrupts the translated
// loads/stores.  Fall back to any pages if the low range is exhausted.
//
STATIC
VOID *
AllocKernelImageBuffer (
  IN UINTN  Size
  )
{
  EFI_PHYSICAL_ADDRESS  Addr;
  EFI_STATUS            Status;
  UINTN                 Pages;

  if (Size == 0) {
    return NULL;
  }
  Pages = EFI_SIZE_TO_PAGES (Size);

  //
  // The 0x389-0x39A MB window (and the 2.4 GB one) proved unreliable on
  // this platform (reads of the same address return different values).
  // Try successively lower ranges: 512 MB, 1 GB, then any pages.
  //
  Addr   = 0x20000000;   // 512 MB
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Addr);
  if (EFI_ERROR (Status)) {
    Addr   = 0x40000000;   // 1 GB
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Addr);
  }
  if (EFI_ERROR (Status)) {
    Status = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, Pages, &Addr);
  }
  if (EFI_ERROR (Status)) {
    return NULL;
  }
  return (VOID *)(UINTN)Addr;
}

//
// Read kernelcache from ZIP file.
// Returns allocated buffer with kernel data, or NULL.
//
STATIC
VOID *
ReadKernelFromZip (
  IN  EFI_FILE_PROTOCOL  *RootDir,
  IN  CONST CHAR16       *ZipPath,
  IN  CONST CHAR16       *EntryName,
  OUT UINT32             *OutSize
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *ZipFile;
  UINT8              EocdBuf[128];
  UINTN              ReadSize;
  UINT64             FileSize;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINT64             EocdOffset;
  UINT64             CentralDirOffset;
  UINT64             CentralDirSize;
  UINT64             TotalEntries;
  UINTN              I;
  UINT32             LocalOffset     = 0;
  UINT32             UncompSize      = 0;
  UINT32             CompressedSize  = 0;
  UINT16             Method       = 0;
  UINT16             NameLen;
  UINT16             ExtraLen;
  UINT8              *Result      = NULL;
  BOOLEAN            EntryFound   = FALSE;

  *OutSize = 0;

  Status = RootDir->Open (RootDir, &ZipFile, (CHAR16 *)ZipPath, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  //
  // Get file size
  //
  InfoSize = 0;
  ZipFile->GetInfo (ZipFile, &gEfiFileInfoGuid, &InfoSize, NULL);
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    ZipFile->Close (ZipFile);
    return NULL;
  }
  Status = ZipFile->GetInfo (ZipFile, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    ZipFile->Close (ZipFile);
    return NULL;
  }
  FileSize = Info->FileSize;
  FreePool (Info);

  if (FileSize < sizeof (EocdBuf)) {
    ZipFile->Close (ZipFile);
    return NULL;
  }

  //
  // Read EOCD from end of file
  //
  ReadSize = sizeof (EocdBuf);
  Status = ZipFile->SetPosition (ZipFile, FileSize - sizeof (EocdBuf));
  if (EFI_ERROR (Status)) {
    ZipFile->Close (ZipFile);
    return NULL;
  }
  Status = ZipFile->Read (ZipFile, &ReadSize, EocdBuf);
  if (EFI_ERROR (Status)) {
    ZipFile->Close (ZipFile);
    return NULL;
  }

  //
  // Find EOCD signature
  //
  EocdOffset = 0;
  for (I = 0; I + 4 < ReadSize; I++) {
    if (*(UINT32 *)(EocdBuf + I) == ZIP_EOCD_SIGNATURE) {
      EocdOffset = FileSize - sizeof (EocdBuf) + I;
      //
      // EOCD fields at this offset:
      // +0: signature(4)
      // +4: disk_number(2)
      // +6: disk_cd(2)
      // +8: entries_on_disk(2)
      // +10: total_entries(2)
      // +12: cd_size(4)
      // +16: cd_offset(4)
      // +20: comment_len(2)
      //
      TotalEntries     = *(UINT16 *)(EocdBuf + I + 10);
      CentralDirSize   = *(UINT32 *)(EocdBuf + I + 12);
      CentralDirOffset = *(UINT32 *)(EocdBuf + I + 16);
      break;
    }
  }

  if (EocdOffset == 0 || CentralDirOffset == 0) {
    ZipFile->Close (ZipFile);
    return NULL;
  }

  //
  // If 32-bit CD offset is 0xFFFFFFFF, use ZIP64 EOCD
  //
  if (CentralDirOffset == 0xFFFFFFFFU) {
    UINT8   Zip64LocBuf[20];
    UINT64  Zip64EocdOff;

    Status = ZipFile->SetPosition (ZipFile, EocdOffset - 20);
    if (!EFI_ERROR (Status)) {
      ReadSize = 20;
      Status = ZipFile->Read (ZipFile, &ReadSize, Zip64LocBuf);
      if (!EFI_ERROR (Status) && (*(UINT32 *)Zip64LocBuf == 0x07064B50U)) {
        Zip64EocdOff = *(UINT64 *)(Zip64LocBuf + 8);
        UINT8   Zip64Buf[56];
        Status = ZipFile->SetPosition (ZipFile, Zip64EocdOff);
        if (!EFI_ERROR (Status)) {
          ReadSize = 56;
          Status = ZipFile->Read (ZipFile, &ReadSize, Zip64Buf);
          if (!EFI_ERROR (Status) && (*(UINT32 *)Zip64Buf == 0x06064B50U)) {
          CentralDirSize   = *(UINT64 *)(Zip64Buf + 40);
            CentralDirOffset = *(UINT64 *)(Zip64Buf + 48);
          }
        }
      }
    }
  }

  //
  // Read central directory
  //
  UINT8  *CdBuf = AllocatePool (CentralDirSize);
  if (CdBuf == NULL) {
    ZipFile->Close (ZipFile);
    return NULL;
  }
  Status = ZipFile->SetPosition (ZipFile, CentralDirOffset);
  if (EFI_ERROR (Status)) {
    FreePool (CdBuf);
    ZipFile->Close (ZipFile);
    return NULL;
  }
  ReadSize = CentralDirSize;
  Status = ZipFile->Read (ZipFile, &ReadSize, CdBuf);
  if (EFI_ERROR (Status)) {
    FreePool (CdBuf);
    ZipFile->Close (ZipFile);
    return NULL;
  }

  //
  // Scan central directory for kernelcache entry
  //
  UINT8  *Ptr = CdBuf;
  for (I = 0; I < TotalEntries; I++) {
    //
    // CFH: sig(4) ver_made(2) ver_need(2) flags(2) method(2) ...
    // +24: comp_size(4), +28: uncomp_size(4), +32: name_len(2), +34: extra_len(2), +36: comment_len(2)
    // +42: local_header_offset(4), +46: filename(N)
    //
    if (*(UINT32 *)Ptr != 0x02014B50U) {
      break;
    }
    Method       = *(UINT16 *)(Ptr + 10);
    UncompSize   = *(UINT32 *)(Ptr + 24);
    NameLen      = *(UINT16 *)(Ptr + 28);
    ExtraLen     = *(UINT16 *)(Ptr + 30);
    LocalOffset  = *(UINT32 *)(Ptr + 42);

    if (NameLen > 0) {
      CHAR8 *Name = (CHAR8 *)(Ptr + 46);
      //
      // Match kernelcache.* files
      //
      if ((NameLen >= 12) && (CompareMem (Name, "AssetData/boot/kernelcache.", 28) == 0)) {
        EntryFound = TRUE;
        DEBUG ((DEBUG_INFO, "DBT: Found kernelcache in ZIP: %a (method=%u, size=%u)\n", Name, Method, UncompSize));
        break;
      }
    }
    Ptr += 46 + NameLen + ExtraLen + *(UINT16 *)(Ptr + 32);
  }
  FreePool (CdBuf);

  if (!EntryFound) {
    ZipFile->Close (ZipFile);
    return NULL;
  }

  //
  // Read local file header to get exact data offset
  //
  UINT8  LocalBuf[128];
  Status = ZipFile->SetPosition (ZipFile, LocalOffset);
  if (EFI_ERROR (Status)) {
    ZipFile->Close (ZipFile);
    return NULL;
  }
  ReadSize = sizeof (LocalBuf);
  Status = ZipFile->Read (ZipFile, &ReadSize, LocalBuf);
  if (EFI_ERROR (Status)) {
    ZipFile->Close (ZipFile);
    return NULL;
  }

  if (*(UINT32 *)LocalBuf != ZIP_LOCAL_FILE_SIGNATURE) {
    ZipFile->Close (ZipFile);
    return NULL;
  }

  NameLen  = *(UINT16 *)(LocalBuf + 26);
  ExtraLen = *(UINT16 *)(LocalBuf + 28);
  UINT32  DataOffset = LocalOffset + 30 + NameLen + ExtraLen;

  if (Method == ZIP_METHOD_STORED) {
    CompressedSize = UncompSize;
  } else {
    CompressedSize = *(UINT32 *)(LocalBuf + 18);
    //
    // Check for ZIP64 extra field if compressed size is 0xFFFFFFFF
    //
    if ((CompressedSize == 0xFFFFFFFFU) && (ExtraLen >= 20)) {
      UINT8   *Extra = (UINT8 *)LocalBuf + 30 + NameLen;
      UINT16  Remaining = ExtraLen;
      while (Remaining >= 4) {
        UINT16  Tag  = *(UINT16 *)Extra;
        UINT16  Size = *(UINT16 *)(Extra + 2);
        if (Tag == 0x0001 && Size >= 16) {  // ZIP64 extra field
          CompressedSize = (UINT32)*(UINT64 *)(Extra + 4 + 8);  // compressed_size after uncompressed_size
          break;
        }
        Remaining = (UINT16)(Remaining - 4 - Size);
        Extra += 4 + Size;
      }
    }
  }

  if (Method == ZIP_METHOD_STORED) {
    //
    // Read uncompressed data directly
    //
    Result = AllocKernelImageBuffer (UncompSize + 1);
    if (Result != NULL) {
      Status = ZipFile->SetPosition (ZipFile, DataOffset);
      if (!EFI_ERROR (Status)) {
        ReadSize = UncompSize;
        Status = ZipFile->Read (ZipFile, &ReadSize, Result);
        if (!EFI_ERROR (Status) && ReadSize == UncompSize) {
          Result[UncompSize] = 0;
          *OutSize = UncompSize;
          DEBUG ((DEBUG_INFO, "DBT: Read kernelcache %u bytes (stored)\n", UncompSize));
        } else {
          FreePool (Result);
          Result = NULL;
        }
      } else {
        FreePool (Result);
        Result = NULL;
      }
    }
  } else {
    //
    // DEFLATE — decompress using zlib inflate
    //
    UINT8  *CompBuf = AllocatePool (CompressedSize);
    if (CompBuf != NULL) {
      Status = ZipFile->SetPosition (ZipFile, DataOffset);
      if (!EFI_ERROR (Status)) {
        ReadSize = CompressedSize;
        Status = ZipFile->Read (ZipFile, &ReadSize, CompBuf);
        if (!EFI_ERROR (Status)) {
          Result = AllocKernelImageBuffer (UncompSize + 1);
          if (Result != NULL) {
            z_stream  Strm;
            INT32     Ret;

            ZeroMem (&Strm, sizeof (Strm));
            Strm.next_in   = CompBuf;
            Strm.avail_in  = (UINT32)ReadSize;
            Strm.next_out  = Result;
            Strm.avail_out = UncompSize;

            Ret = inflateInit2 (&Strm, -MAX_WBITS);
            if (Ret == Z_OK) {
              Ret = inflate (&Strm, Z_FINISH);
              inflateEnd (&Strm);
              if ((Ret == Z_STREAM_END) && (Strm.total_out == UncompSize)) {
                Result[UncompSize] = 0;
                *OutSize = UncompSize;
                DEBUG ((DEBUG_INFO, "DBT: Decompressed kernelcache %u bytes (deflate)\n", UncompSize));
              } else {
                DEBUG ((DEBUG_INFO, "DBT: inflate failed ret=%d total_out=%lu expected=%u\n", Ret, (UINT64)Strm.total_out, UncompSize));
                FreePool (Result);
                Result = NULL;
              }
            } else {
              DEBUG ((DEBUG_INFO, "DBT: inflateInit2 failed ret=%d\n", Ret));
              FreePool (Result);
              Result = NULL;
            }
          }
        }
      }
      FreePool (CompBuf);
    }
  }

  ZipFile->Close (ZipFile);
  return Result;
}

STATIC
BOOLEAN
IsArm64BootEfi (
  IN  EFI_FILE_PROTOCOL  *RootDirectory
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINT8              Header[128];
  UINTN              ReadSize;
  UINT32             PeOffset;
  UINT16             Machine;

  Status = RootDirectory->Open (
                           RootDirectory,
                           &File,
                           L"\\System\\Library\\CoreServices\\boot.efi",
                           EFI_FILE_MODE_READ,
                           0
                           );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  ReadSize = sizeof (Header);
  Status   = File->Read (File, &ReadSize, Header);
  File->Close (File);

  if (EFI_ERROR (Status) || (ReadSize < 64) || (*(UINT16 *)Header != 0x5A4D)) {
    return FALSE;
  }

  PeOffset = *(UINT32 *)(Header + 0x3C);
  if ((PeOffset + 6) > ReadSize) {
    return FALSE;
  }

  if (*(UINT32 *)(Header + PeOffset) != 0x00004550) {
    return FALSE;
  }

  Machine = *(UINT16 *)(Header + PeOffset + 4);
  if (Machine == 0xAA64) {
    DEBUG ((DEBUG_INFO, "DBT: ARM64 boot.efi found, kernel must boot through DBT\n"));
    return TRUE;
  }

  return FALSE;
}

//
// Enumerate a directory for the kernel cache.  Recent installers name the
// cache after the board (kernelcache.release.mac15j, mac16j, ...), so the
// name must not be hardcoded.
//
STATIC
BOOLEAN
FindKernelInDirectory (
  IN  EFI_FILE_PROTOCOL  *Directory,
  IN  CONST CHAR16       *DirectoryPath,
  OUT CHAR16             *KernelPath,
  IN  UINTN              KernelPathSize
  )
{
  EFI_STATUS     Status;
  EFI_FILE_INFO  *Info;
  UINT8          *Buffer;
  UINTN          BufferSize;
  UINTN          ReadSize;

  BufferSize = SIZE_256KB;
  Buffer     = AllocatePool (BufferSize);
  if (Buffer == NULL) {
    return FALSE;
  }

  for (;;) {
    ReadSize = BufferSize;
    Status   = Directory->Read (Directory, &ReadSize, Buffer);
    if (EFI_ERROR (Status) || (ReadSize == 0)) {
      break;
    }

    Info = (EFI_FILE_INFO *)Buffer;
    while ((UINTN)Info < (UINTN)Buffer + ReadSize) {
      if (Info->Size == 0) {
        break;
      }

      if (  ((Info->Attribute & EFI_FILE_DIRECTORY) == 0)
         && (StrnCmp (Info->FileName, L"kernelcache.", 12) == 0))
      {
        UnicodeSPrint (KernelPath, KernelPathSize, L"%s\\%s", DirectoryPath, Info->FileName);
        FreePool (Buffer);
        return TRUE;
      }

      Info = (EFI_FILE_INFO *)((UINT8 *)Info + Info->Size);
    }
  }

  FreePool (Buffer);
  return FALSE;
}

//
// Find the macOS kernel / kernel cache on an installer or SharedSupport
// volume.  Both the classic layouts and the MobileAsset layout used by
// recent installers are probed.
//
STATIC
BOOLEAN
FindInstallerKernel (
  IN  EFI_FILE_PROTOCOL  *RootDirectory,
  OUT CHAR16             *KernelPath,
  IN  UINTN              KernelPathSize
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_PROTOCOL  *Directory;
  UINTN              Index;

  STATIC CONST CHAR16  *KernelPaths[] = {
    L"\\SharedSupport\\kernel",
    L"\\SharedSupport\\kernelcache.release",
    L"\\SharedSupport\\kernelcache.decomp",
    L"\\kernelcache.decomp",
    L"\\kernelcache.release",
    L"\\kernel",
    L"\\System\\Library\\Kernels\\kernel",
    L"\\System\\Library\\KernelCache\\kernelcache.release",
    NULL
  };

  STATIC CONST CHAR16  *KernelDirs[] = {
    L"\\com_apple_MobileAsset_MacSoftwareUpdate\\AssetData\\boot",
    L"\\AssetData\\boot",
    L"\\SharedSupport",
    NULL
  };

  for (Index = 0; KernelPaths[Index] != NULL; ++Index) {
    Status = RootDirectory->Open (
                             RootDirectory,
                             &File,
                             (CHAR16 *)KernelPaths[Index],
                             EFI_FILE_MODE_READ,
                             0
                             );
    if (!EFI_ERROR (Status)) {
      File->Close (File);
      UnicodeSPrint (KernelPath, KernelPathSize, L"%s", KernelPaths[Index]);
      return TRUE;
    }
  }

  for (Index = 0; KernelDirs[Index] != NULL; ++Index) {
    Status = RootDirectory->Open (
                             RootDirectory,
                             &Directory,
                             (CHAR16 *)KernelDirs[Index],
                             EFI_FILE_MODE_READ,
                             0
                             );
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (FindKernelInDirectory (Directory, KernelDirs[Index], KernelPath, KernelPathSize)) {
      Directory->Close (Directory);
      return TRUE;
    }

    Directory->Close (Directory);
  }

  return FALSE;
}

//
// Locate the volume that actually holds the installer kernel.  On recent
// installers the ARM64 boot.efi lives on the installer volume while the
// x86_64 kernel and dyld shared cache live on a separate SharedSupport
// volume, so every SimpleFileSystem handle is probed.
//
STATIC
EFI_HANDLE
FindInstallerKernelDevice (
  IN  EFI_HANDLE  PreferredDevice OPTIONAL,
  OUT CHAR16      *KernelPath,
  IN  UINTN       KernelPathSize
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_HANDLE                       Result;

  KernelPath[0] = L'\0';
  Result        = NULL;

  if (PreferredDevice != NULL) {
    Status = gBS->HandleProtocol (
                    PreferredDevice,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (!EFI_ERROR (Status)) {
      Status = FileSystem->OpenVolume (FileSystem, &Root);
      if (!EFI_ERROR (Status)) {
        if (FindInstallerKernel (Root, KernelPath, KernelPathSize)) {
          Result = PreferredDevice;
        }

        Root->Close (Root);
      }
    }

    if (Result != NULL) {
      return Result;
    }
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Count == 0)) {
    return NULL;
  }

  for (Index = 0; Index < Count; ++Index) {
    if (Handles[Index] == PreferredDevice) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = FileSystem->OpenVolume (FileSystem, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (FindInstallerKernel (Root, KernelPath, KernelPathSize)) {
      Result = Handles[Index];
      Root->Close (Root);
      break;
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return Result;
}

STATIC
BOOLEAN
#if defined (__GNUC__) || defined (__clang__)
__attribute__((unused))
#endif
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
    CpuType = Header64->CpuType;
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

//
// Minimal device tree structure for XNU handoff
//
#pragma pack(push, 1)
typedef struct {
  UINT32  NumProperties;
  UINT32  NumChildren;
} DT_NODE;

typedef struct {
  CHAR8   Name[32];
  UINT32  Length;
} DT_PROP;
#pragma pack(pop)

STATIC
EFI_STATUS
CreateMinimalDeviceTree (
  OUT UINT8   **DeviceTree,
  OUT UINTN   *DeviceTreeSize
  )
{
  //
  // Simple flattened device tree with /chosen node containing boot-args
  // Will be allocated and populated at runtime
  //
  UINTN  Size = EFI_PAGE_SIZE;
  EFI_STATUS  Status;
  
  Status = gBS->AllocatePool (EfiBootServicesData, Size, (VOID **)DeviceTree);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  ZeroMem (*DeviceTree, Size);
  *DeviceTreeSize = Size;
  
  //
  // Create minimal /chosen node
  // Format: DT_NODE { NumProperties, NumChildren } [0,0 ends]
  // followed by properties
  //
  DT_NODE  *Root = (DT_NODE *)*DeviceTree;
  Root->NumProperties = 1;
  Root->NumChildren = 0;
  
  DT_PROP  *Prop = (DT_PROP *)((UINTN)Root + sizeof (DT_NODE));
  AsciiStrCpyS (Prop->Name, 32, "boot-args");
  Prop->Length = 2;  // "1"
  *((CHAR8 *)((UINTN)Prop + sizeof (DT_PROP))) = '1';
  
  return EFI_SUCCESS;
}

//
// xnu's arm64 boot code (osfmk/arm64/start.s) scans a per-CPU data entries
// table that the bootloader is expected to have populated before jumping to
// the kernel:
//
//   mrs  x15, mpidr_el1
//   and  x0, x15, #0xff
//   adr  x1, <entries table>
//   add  x1, x1, #<pageoff>
//   ...
//   ldr  x21, [x1, #8]              // per-CPU data address of the entry
//   cbz  x21, .                     // spin forever if not set up
//   ldr  w2, [x21, #0x1c8]          // CPU_PHYS_ID
//   cmp  x0, x2
//   b.eq <found>
//
// On real hardware iBoot fills the table before starting the kernel; with a
// direct load the slots contain file data (observed: 0x201FD503201FD503),
// which makes the kernel dereference garbage and fault.  We locate the table
// by decoding the ADR/ADD pair right after the MRS/AND prologue and route the
// entry to a fake per-CPU data structure that lives inside the kernel image.
// xnu continues the handoff with strict VA checks (stack fields and a reset
// handler constant), so a separate host buffer can never pass them.
//
STATIC
UINT8 *
SetupCpuDataEntries (
  IN UINT8   *KernelBuffer,
  IN UINTN   SegCount,
  IN UINT64  *SegVmAddr,
  IN UINT64  *SegVmSize,
  IN UINT64  *SegFileOff,
  IN UINT64  EntryPoint,
  IN UINT8   *StackBase,
  IN UINTN   StackSize
  )
{
  UINTN   I;
  UINTN   J;
  UINTN   K;
  UINTN   L;
  UINTN   FirstSeg;
  UINT8   *CpuData = NULL;

  if (SegCount == 0) {
    return NULL;
  }

  FirstSeg = 0;
  for (I = 0; I < SegCount; I++) {
    if (EntryPoint >= SegVmAddr[I] && EntryPoint < SegVmAddr[I] + SegVmSize[I]) {
      FirstSeg = I;
      break;
    }
  }

  for (K = 0; K < 2 && CpuData == NULL; K++) {
    UINTN  SegStart = K == 0 ? FirstSeg : 0;
    UINTN  SegEnd   = K == 0 ? FirstSeg + 1 : SegCount;

    for (I = SegStart; I < SegEnd && CpuData == NULL; I++) {
      UINT8  *Seg = KernelBuffer + SegFileOff[I];
      UINTN  Len  = (UINTN)SegVmSize[I];

      for (J = 0; J + 16 <= Len && CpuData == NULL; J += 4) {
        UINT32  Inst;
        UINT64  Pc;
        UINT64  Addr;
        UINT64  TableVa;
        UINT64  Imm;
        UINT8   *HostTable;

        if (*(UINT32 *)(Seg + J) != 0xD53800AF) {          // mrs x15, mpidr_el1
          continue;
        }
        if (*(UINT32 *)(Seg + J + 4) != 0x92403DE0) {      // and x0, x15, #0xff
          continue;
        }
Inst = *(UINT32 *)(Seg + J + 8);
        if ((Inst & 0x1F000000) != 0x10000000 ||   // (adr|adrp) prologue
            (Inst & 0x1F) != 1) {
          continue;
        }
        Pc   = SegVmAddr[I] + J;
        Imm  = (((UINT64)((Inst >> 5) & 0x7FFFF)) << 2) | ((UINT64)((Inst >> 29) & 3));
        if (Imm & (1ULL << 20)) {
          Imm |= ~((UINT64)0x1FFFFF);
        }
        if ((Inst & 0x9F000000) == 0x90000000) {   // ADRP (not ADR)
          Addr = (Pc + (Imm << 12)) & ~((UINT64)0xFFF);
        } else {
          Addr = Pc + Imm;                         // ADR
        }
        Inst = *(UINT32 *)(Seg + J + 12);
        if ((Inst & 0xFF800000) != 0x91000000 ||           // add x1, x1, #imm
            (Inst & 0x3FF) != 0x21) {
          continue;
        }
        TableVa = Addr + ((Inst >> 10) & 0xFFF);

        HostTable = NULL;
        for (L = 0; L < SegCount; L++) {
          if (TableVa >= SegVmAddr[L] && TableVa < SegVmAddr[L] + SegVmSize[L]) {
            HostTable = KernelBuffer + SegFileOff[L] + (TableVa - SegVmAddr[L]);
            break;
          }
        }
        if (HostTable == NULL) {
          break;
        }

        // The kernel's per-CPU data handoff is fully VA-relative: after the
        // scan sets SP from cpu_data[+0x18] (machine stack) and [+0x28]
        // (exception stack) it reads cpu_data[+0xB8] and requires it to equal
        // one of two fixed kernel addresses (here EntryPoint+0x77B or
        // EntryPoint-0x420); anything else spins on DEADB001/DEADB002.  A
        // separate host heap buffer can never match those constants, so the
        // fake cpu_data lives inside the __TEXT_BOOT_EXEC NOP padding
        // (EntryPoint+0x77B is all NOPs).
        //
        {
          UINT64  CpuDataVa;
          UINT64  ResetHandler;
          UINTN   Pad;
          UINT8   *CpuDataHost;

          //
          // The kernel checks cpu_data[+0xB8] for equality against two fixed
          // kernel addresses that __TEXT_BOOT_EXEC builds with ADRP/ADD right
          // after the MRS/scan prologue (observed for macOS 27:
          // 0xFFFFFE000BD4FE04 and 0xFFFFFE000BD50258).  Decode both from the
          // two ADRP/ADD pairs that load x3 after the `ldr x2,[x21,#0xB8]`
          // check; the first candidate is the equality target of the first
          // branch.  On failure fall back to the legacy EntryPoint+0x77B.
          //
          {
            UINT64  ResetCandidates[2];
            UINTN   RC;

            ResetCandidates[0] = 0;
            ResetCandidates[1] = 0;
            for (RC = 0; RC < 2; RC++) {
              UINT32  RInst8;
              UINT64  RPc;
              UINT64  RImm;
              UINT64  RAddr;
              UINT32  RInst12;

              if (J + 0x94 > Len) {
                break;
              }

              //
              // Reset-handler check sequence (relative to master slot J);
              // disassembly of the macOS 27 prologue:
              //   J+0x70: ldr  x2, [x21, #0xb8]
              //   J+0x74: cbz  x2, dead
              //   J+0x78: adrp x3                    (candidate #0)
              //   J+0x7C: add  x3, x3, #imm
              //   J+0x80: cmp  x2, x3
              //   J+0x84: b.eq <handler0>
              //   J+0x88: adrp x3                    (candidate #1)
              //   J+0x8C: add  x3, x3, #imm
              //   J+0x90: cmp  x2, x3
              //   J+0x94: b.eq <handler1>
              //
              RInst8 = *(UINT32 *)(Seg + J + 0x78 + (RC * 0x10));
              if ((RInst8 & 0x1F000000) != 0x10000000 ||
                  (RInst8 & 0x1F) != 3) {
                continue;
              }
              RPc  = SegVmAddr[I] + J + 0x78 + (RC * 0x10);
              RImm = (((UINT64)((RInst8 >> 5) & 0x7FFFF)) << 2) |
                     ((UINT64)((RInst8 >> 29) & 3));
              if (RImm & (1ULL << 20)) {
                RImm |= ~((UINT64)0x1FFFFF);
              }
              if ((RInst8 & 0x9F000000) == 0x90000000) {
                RAddr = (RPc + (RImm << 12)) & ~((UINT64)0xFFF);
              } else {
                RAddr = RPc + RImm;
              }
              RInst12 = *(UINT32 *)(Seg + J + 0x7C + (RC * 0x10));
              if ((RInst12 & 0xFF800000) != 0x91000000 ||
                  (RInst12 & 0x3FF) != 0x63) {
                continue;
              }
              ResetCandidates[RC] = RAddr + ((RInst12 >> 10) & 0xFFF);
            }

            ResetHandler = 0;
            for (RC = 0; RC < 2; RC++) {
              if (ResetCandidates[RC] != 0) {
                ResetHandler = ResetCandidates[RC];
                break;
              }
            }
            if (ResetHandler == 0) {
              ResetHandler = EntryPoint + 0x77B;
            }
          }

          CpuDataVa     = EntryPoint + 0x77B;
          CpuDataHost   = KernelBuffer + SegFileOff[FirstSeg] + 0x77B;
          Pad = SegVmSize[FirstSeg] > 0x77B + 0x1000 ? 0x1000 : 0;

          if (Pad != 0) {
            ZeroMem (CpuDataHost, Pad);
          }

          //
          // CPU_PHYS_ID at +0x1C8 must match MPIDR_EL1 & 0xff (= 0 with the
          // virtual MPIDR reported by the DBT).
          //
          *(UINT32 *)(CpuDataHost + 0x1C8) = 0;
          //
          // CPU number at +0x1B8: the kernel indexes a per-CPU slot table
          // with it (`ldr w, [cpu_data,#0x1b8]; add x8,x8,x,w,uxt#4`), so it
          // must be 0 for the single emulated CPU to hit slot 0.
          //
          *(UINT32 *)(CpuDataHost + 0x1B8) = 0;

          //
          // Stacks: the kernel does 'mov sp, cpu_data[+0x18]' (intstack) and
          // again 'mov sp, cpu_data[+0x28]' (excepstack), so both must be
          // usable host addresses (identity-mapped by the DBT).
          //
          *(UINT64 *)(CpuDataHost + 0x18) = (UINT64)(UINTN)StackBase + StackSize;
          *(UINT64 *)(CpuDataHost + 0x28) = (UINT64)(UINTN)StackBase + StackSize;

          //
          // The +0xB8 reset handler is only checked for being nonzero (the
          // zero case falls into the DEADB001 spin), a value is enough.
          //
          *(UINT64 *)(CpuDataHost + 0xB8) = ResetHandler;
          CpuData = CpuDataHost;

          DEBUG ((DEBUG_INFO, "DirectKernel: cpu data entries table va=0x%llx host=%p\n",
                  TableVa, HostTable));
          DEBUG ((DEBUG_INFO, "DirectKernel: fake cpu_data va=0x%llx host=%p phys-id=%u\n",
                   CpuDataVa, CpuData, *(UINT32 *)(CpuData + 0x1C8)));
        }

        //
        // Write the handoff: entry[0] carries the per-CPU vaddr (kernel reads
        // the +8 slot) as a VA inside the kernel image, and the remaining 31
        // entries are cleared so the scan cannot dereference stale file data.
        //
        *(UINT64 *)(HostTable + 0) = (UINT64)EntryPoint + 0x77B;
        *(UINT64 *)(HostTable + 8) = (UINT64)EntryPoint + 0x77B;
        ZeroMem (HostTable + 16, 32 * 16 - 16);

        break;
      }
    }
  }

  return CpuData;
}

//
// --------------------------- x86_64 direct kernel boot ---------------------------
//
// The macOS "Golden Gate" (27.x) restore FileSystem volumes carry a PRELINKED
// x86_64 kernel cache at \System\Library\Kernels\kernel (Mach-O with
// __PRELINK_TEXT/__PRELINK_INFO segments, filetype=2). On an x86_64 host this
// kernel boots natively without Apple's boot.efi:
//
//  1. re-host the kernel image below 4 GB (BootArgs2.kaddr is UINT32),
//  2. build fresh x86_64 page tables that identity-map every memory-map
//     range (so UEFI runtime structures and the handoff stack stay valid) and
//     map each Mach-O LC_SEGMENT_64 VA range to its host physical address
//     (slide 0 — the kernel runs at its linked addresses),
//  3. build a BootArgs2 structure with the EFI memory map, the framebuffer,
//     PCI ECAM and the kernel cache header virtual address,
//  4. provide a < 4 GB copy of the EFI System Table + Runtime Services
//     (BootArgs2.efiSystemTable is UINT32 on x86),
//  5. ExitBootServices(), then jump straight to the LC_UNIXTHREAD entry
//     point. Apple's boot.efi hands xnu the boot args pointer in %rsi; the
//     stub also places it in %rdi and %rdx so every caller convention works.
//
typedef struct {
  UINT64  *Pml4;
} DBT_X86_PAGE_TABLE;

#define DBT_X86_PTE_PRESENT  0x01
#define DBT_X86_PTE_WRITABLE 0x02

//
// Allocate (or reuse) the next page-table level pointed to by *Slot.
// Returns the table, or NULL on allocation failure.
//
STATIC
UINT64 *
X86PtNextLevel (
  DBT_X86_PAGE_TABLE  *PageTable,
  UINT64              *Slot
  )
{
  UINT64               *Table;
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;

  Table = (UINT64 *)(UINTN)(*Slot & 0xFFFFFFFFFFFFF000ULL);
  if (Table != NULL) {
    return Table;
  }

  Status = gBS->AllocatePages (AllocateAnyPages, EfiReservedMemoryType, 1, &Addr);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Table = (UINT64 *)(UINTN)Addr;
  ZeroMem (Table, EFI_PAGE_SIZE);
  *Slot = (UINT64)(UINTN)Table | DBT_X86_PTE_PRESENT | DBT_X86_PTE_WRITABLE;
  return Table;
}

//
// Identity/mapped page: 4 KB page via PML4/PDPT/PD/PT.
//
STATIC
BOOLEAN
X86PtMap4K (
  DBT_X86_PAGE_TABLE  *PageTable,
  UINT64              Virtual,
  UINT64              Physical
  )
{
  UINT64  *Pdpt;
  UINT64  *Pd;
  UINT64  *Pt;

  Pdpt = X86PtNextLevel (PageTable, &PageTable->Pml4[(Virtual >> 39) & 0x1FF]);
  if (Pdpt == NULL) {
    return FALSE;
  }

  Pd = X86PtNextLevel (PageTable, &Pdpt[(Virtual >> 30) & 0x1FF]);
  if (Pd == NULL) {
    return FALSE;
  }

  Pt = X86PtNextLevel (PageTable, &Pd[(Virtual >> 21) & 0x1FF]);
  if (Pt == NULL) {
    return FALSE;
  }

  Pt[(Virtual >> 12) & 0x1FF] = (Physical & 0x000FFFFFFFFFF000ULL) |
    DBT_X86_PTE_PRESENT | DBT_X86_PTE_WRITABLE;
  return TRUE;
}

//
// 2 MB page (PS set) — used for identity mapping physical RAM.
//
STATIC
BOOLEAN
X86PtMap2M (
  DBT_X86_PAGE_TABLE  *PageTable,
  UINT64              Virtual,
  UINT64              Physical
  )
{
  UINT64  *Pdpt;
  UINT64  *Pd;
  UINT64  Slot;
  UINTN   Index;

  Pdpt = X86PtNextLevel (PageTable, &PageTable->Pml4[(Virtual >> 39) & 0x1FF]);
  if (Pdpt == NULL) {
    return FALSE;
  }

  Pd = X86PtNextLevel (PageTable, &Pdpt[(Virtual >> 30) & 0x1FF]);
  if (Pd == NULL) {
    return FALSE;
  }

  Index = (Virtual >> 21) & 0x1FF;
  Slot  = Pd[Index];

  //
  // Never split an existing 4 KB leaf table into a 2 MB page — if the slot
  // already points at a PT (allocated by Map4K, e.g. for the kernel image),
  // route this call through a 4 KB walk instead.
  //
  if ((Slot & (0x80 | 0xFFFFFFFFFFFFF000ULL)) != 0 &&
      !(Slot & 0x80))
  {
    for (Index = 0; Index < 512; ++Index) {
      UINT64  Pa = (Physical & ~(0x1FFFFFULL)) + Index * 0x1000ULL;
      if (!X86PtMap4K (PageTable, Virtual + Index * 0x1000ULL, Pa)) {
        return FALSE;
      }
    }
    return TRUE;
  }

  Pd[Index] = (Physical & ~(UINT64)0x1FFFFFULL) |
    DBT_X86_PTE_PRESENT | DBT_X86_PTE_WRITABLE | 0x80 /* PS */;
  return TRUE;
}

//
// Identity-map [Base, End) with 2 MB pages (rounded outwards).
//
STATIC
BOOLEAN
X86PtIdentityRange (
  DBT_X86_PAGE_TABLE  *PageTable,
  UINT64              Base,
  UINT64              End
  )
{
  Base &= ~((UINT64)0x1FFFFF);
  End   = (End + 0x1FFFFF) & ~((UINT64)0x1FFFFF);

  while (Base < End) {
    if (!X86PtMap2M (PageTable, Base, Base)) {
      return FALSE;
    }
    Base += 0x200000ULL;
  }
  return TRUE;
}

//
// Allocate `Pages` pages strictly below 4 GB (BootArgs2 fields are UINT32).
//
STATIC
EFI_STATUS
X86AllocLow (
  IN  UINTN   Pages,
  OUT UINT8   **Out
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;

  Addr   = 0x100000000ULL;   /* 4 GB */
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiLoaderData, Pages, &Addr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Out = (UINT8 *)(UINTN)Addr;
  return EFI_SUCCESS;
}

//
// Copy the EFI System Table (+ Runtime Services + configuration table
// array) into a reserved < 4 GB block so that BootArgs2.efiSystemTable
// (UINT32) stays addressable from xnu. Internal pointers point at the
// copies; the boot services + console pointers are cleared as they are
// meaningless after ExitBootServices.
//
STATIC
EFI_STATUS
X86CopyEfiSystemTable (
  OUT UINT64  *TablePhys
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;
  UINTN                 BlockSize;
  UINTN                 Pages;
  EFI_SYSTEM_TABLE      *St;
  EFI_RUNTIME_SERVICES  *Rs;
  EFI_CONFIGURATION_TABLE *Table;

  BlockSize = sizeof (EFI_SYSTEM_TABLE) + sizeof (EFI_RUNTIME_SERVICES) +
              gST->NumberOfTableEntries * sizeof (EFI_CONFIGURATION_TABLE) +
              EFI_PAGE_SIZE;
  Pages = EFI_SIZE_TO_PAGES (BlockSize);

  Addr   = 0x100000000ULL;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, Pages, &Addr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  St = (EFI_SYSTEM_TABLE *)(UINTN)Addr;
  CopyMem (St, gST, sizeof (EFI_SYSTEM_TABLE));

  Rs              = (EFI_RUNTIME_SERVICES *)((UINT8 *)St + sizeof (EFI_SYSTEM_TABLE));
  CopyMem (Rs, gST->RuntimeServices, sizeof (EFI_RUNTIME_SERVICES));
  St->RuntimeServices = Rs;
  St->BootServices    = NULL;

  Table = (EFI_CONFIGURATION_TABLE *)((UINT8 *)Rs + sizeof (EFI_RUNTIME_SERVICES));
  CopyMem (Table, gST->ConfigurationTable,
           gST->NumberOfTableEntries * sizeof (EFI_CONFIGURATION_TABLE));
  St->ConfigurationTable = Table;

  St->ConsoleInHandle  = NULL;
  St->ConIn            = NULL;
  St->ConsoleOutHandle = NULL;
  St->ConOut           = NULL;
  St->StandardErrorHandle = NULL;
  St->StdErr           = NULL;

  *TablePhys = (UINT64)(UINTN)Addr;
  return EFI_SUCCESS;
}

//
// Capture the current EFI memory map into a reserved, page-aligned,
// < 4 GB buffer. Returns the buffer and the descriptor geometry. The map is
// what xnu uses to learn about available memory (BootArgs2.MemoryMap is
// UINT32 physical).
//
STATIC
EFI_STATUS
X86CaptureMemoryMap (
  OUT UINT8   **MapBuffer,
  OUT UINTN   *MapSize,
  OUT UINTN   *DescriptorSize,
  OUT UINTN   *DescriptorVersion,
  OUT UINT64  *PhysicalMemorySize
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;
  UINTN                 Size;
  UINTN                 DescSize;
  UINT32                DescVersion;
  UINTN                 Key;
  UINTN                 Attempt;

  *MapBuffer = NULL;
  Size       = 0;
  DescSize   = 0;
  DescVersion = 0;

  for (Attempt = 0; Attempt < 8; ++Attempt) {
    UINT8   *Buffer;
    UINTN   Pages;

    Size = 0;
    Status = gBS->GetMemoryMap (&Size, NULL, &Key, &DescSize, &DescVersion);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
    }

    if (DescSize == 0) {
      DescSize = sizeof (EFI_MEMORY_DESCRIPTOR);
    }

    Size += 4 * DescSize;   /* slack for allocations that happen meanwhile */
    Pages = EFI_SIZE_TO_PAGES (Size);

    Addr   = 0x100000000ULL;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, Pages, &Addr);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Buffer = (UINT8 *)(UINTN)Addr;
    Size   = EFI_PAGES_TO_SIZE (Pages);

    Status = gBS->GetMemoryMap (&Size, (EFI_MEMORY_DESCRIPTOR *)Buffer, &Key, &DescSize, &DescVersion);
    if (!EFI_ERROR (Status)) {
      UINTN                 Offset;
      EFI_MEMORY_DESCRIPTOR *Desc;

      *MapBuffer         = Buffer;
      *MapSize           = Size;
      *DescriptorSize    = DescSize;
      *DescriptorVersion = DescVersion;

      //
      // Total usable RAM: highest end of conventional memory.
      //
      *PhysicalMemorySize = 0;
      for (Offset = 0; Offset < Size; Offset += DescSize) {
        Desc = (EFI_MEMORY_DESCRIPTOR *)(Buffer + Offset);
        if (Desc->Type == EfiConventionalMemory) {
          UINT64  End = Desc->PhysicalStart + (UINT64)Desc->NumberOfPages * 4096ULL;
          if (End > *PhysicalMemorySize) {
            *PhysicalMemorySize = End;
          }
        }
      }

      return EFI_SUCCESS;
    }

    gBS->FreePages (Addr, Pages);
  }

  return EFI_OUT_OF_RESOURCES;
}

//
// Final handoff: switch page tables, disable interrupts, jump to the entry
// point. The boot args pointer is passed in %rsi (expected by osfmk/i386
// start.s after the same call boot.efi makes) and mirrored in %rdi and %rdx.
// This function never returns.
//
//
// X86JumpKernel — switches page tables (CR3), places the boot-args pointer in
// %rsi/%rdi/%rdx (Apple's boot.efi convention, mirrored for safety) and jumps
// straight to the xnu entry point. Never returns.
//
// Implemented as inline asm for clang/GCC (our XCODE5/Docker toolchains). For
// MSVC x64 (no inline asm) we fall back to _WriteCR3 + indirect call — the
// boot-args register convention is not honored there, but the module at least
// builds on Windows CI.
//
//
// X86JumpKernel — switches page tables (CR3), places the boot-args pointer in
// %rsi/%rdi/%rdx (Apple's boot.efi convention, mirrored for safety) and jumps
// straight to the xnu entry point. Never returns.
//
// Only compiled for x86_64 targets. IA32 builds fall through to the generic
// EFI_UNSUPPORTED path below.
//

#if defined(__x86_64__) || defined(_M_X64)

#if defined(_MSC_VER)
//
// MSVC x64 does not support inline asm, so this is a best-effort stub that at
// least builds on Windows CI. The real handoff (with boot-args in %rsi/%rdi/%rdx)
// requires clang/GCC inline asm.
//
STATIC
VOID
EFIAPI
X86JumpKernel (
  IN UINT64  Cr3,
  IN UINT64  EntryPoint,
  IN UINT64  BootArgsPhys
  )
{
  ((VOID (EFIAPI *)(VOID))(VOID *)EntryPoint) ();
  CpuDeadLoop ();
}
#else
//
// clang/GCC inline asm — used on macOS XCODE5 and Linux GCC Docker builds.
//
STATIC
VOID
EFIAPI
X86JumpKernel (
  IN UINT64  Cr3,
  IN UINT64  EntryPoint,
  IN UINT64  BootArgsPhys
  )
{
  asm volatile (
    "cli\n\t"
    "movq %%rcx, %%cr3\n\t"
    "movq %3, %%rsi\n\t"
    "movq %3, %%rdi\n\t"
    "movq %3, %%rdx\n\t"
    "jmpq *%%rax\n\t"
    : /* no outputs */
    : "c" (Cr3), "a" (EntryPoint), "d" (BootArgsPhys), "g" (BootArgsPhys)
    : "memory"
  );

  //
  // A well-behaved kernel never returns.
  //
  CpuDeadLoop ();
}
#endif

#endif // defined(__x86_64__) || defined(_M_X64)

STATIC
EFI_STATUS
DirectLoadKernel (
  IN  OC_PICKER_CONTEXT  *PickerContext
  )
{
  EFI_STATUS                       Status;
  UINTN                            VerboseStep = 0;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *RootDirectory;
  EFI_FILE_PROTOCOL                *KernelFile;
  UINT8                            *KernelBuffer   = NULL;
  UINT32                           KernelSize      = 0;
  UINT32                           AllocatedSize   = 0;
  BOOLEAN                          Is32Bit         = FALSE;
  OC_MACHO_CONTEXT                 MachoContext;
  UINT64                           EntryPoint;
  BootArgs2                       *BootArgs;
  UINTN                            BootArgsSize;
  BOOLEAN                          IsArm64;
  UINT8                            *StackBuffer;
  UINTN                            StackSize;
  UINT8                            *DeviceTreeBuffer;
  UINTN                            DeviceTreeSize;
  UINTN                            Index;
  MACH_LOAD_COMMAND                *Cmd;
  MACH_LOAD_COMMAND                *CmdEnd;
  MACH_HEADER_64                   *Header64;
  UINT64                           SegVmAddr[32];
  UINT64                           SegVmSize[32];
  UINT64                           SegFileOff[32];
  UINTN                            SegCount       = 0;
  EFI_HANDLE                       Device;
  CONST CHAR16                     *KernelPath;

  DEBUG ((DEBUG_VERBOSE, "DBT_LOADER: DirectLoadKernel entry — Picker=%p VerboseStep=%u\n",
          PickerContext, VerboseStep++));

  if (gDbtContext == NULL) {
    DEBUG ((DEBUG_ERROR, "DBT_LOADER: DBT context is NULL — abort\n"));
    return EFI_NOT_STARTED;
  }

  Device     = DbtGetInstallerDevice (gDbtContext);
  KernelPath = DbtGetKernelPath (gDbtContext);

  DEBUG ((DEBUG_INFO, "DirectKernel: device=%p path=%s\n",
          Device, KernelPath != NULL ? KernelPath : L"<null>"));

  //
  // Auto-discovery: when no installer boot info is bound yet, scan the
  // SimpleFileSystem handles for the macOS installer / SharedSupport volume
  // and bind the translated boot to it. This lets the original macOS kernel
  // (and its dyld shared cache) be located automatically, without requiring
  // any manual kernel extraction step.
  //
  DEBUG ((DEBUG_VERBOSE, "DBT_LOADER: Auto-discovery start — Device=%p KernelPath=%ls Step=%u\n",
          Device, KernelPath ? KernelPath : L"<null>", VerboseStep++));
  if ((gDbtContext != NULL) && (Device == NULL)) {
    CHAR16      KernelPathBuf[256];
    EFI_HANDLE  KernelDevice;

    //
    // Probe every SimpleFileSystem handle for a kernel.  The kernel is
    // commonly on a separate SharedSupport volume, so looking only at the
    // volume that carries boot.efi is not enough.
    //
    KernelDevice = FindInstallerKernelDevice (NULL, KernelPathBuf, sizeof (KernelPathBuf));
    if (KernelDevice != NULL) {
      Device           = KernelDevice;
      gInstallerDevice = KernelDevice;
      DbtSetBootInfo (gDbtContext, KernelDevice, KernelPathBuf);
      KernelPath = DbtGetKernelPath (gDbtContext);
      DEBUG ((DEBUG_INFO, "DirectKernel: installer kernel auto-detected at %s\n", KernelPathBuf));
    } else {
      DEBUG ((DEBUG_WARN, "DirectKernel: no installer kernel found on any volume\n"));
    }
  }

  if (Device == NULL || KernelPath == NULL) {
    DEBUG ((DEBUG_ERROR, "DirectKernel: No boot info set\n"));
    return EFI_NOT_STARTED;
  }

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

  //
  // Try multiple kernel paths: the stored path first, then fallbacks
  //
  {
    STATIC CONST CHAR16  *FallbackPaths[] = {
      L"\\SharedSupport\\kernel",
      L"\\SharedSupport\\kernelcache.release",
      L"\\SharedSupport\\kernelcache.decomp",
      L"\\kernelcache.decomp",
      L"\\kernelcache.release",
      L"\\kernel",
      L"\\System\\Library\\Kernels\\kernel",
      L"\\System\\Library\\KernelCache\\kernelcache.release",
      NULL
    };
    UINTN  PathIndex;
    CONST CHAR16  *TryPath;

    KernelFile = NULL;

    TryPath = KernelPath;
    Status = RootDirectory->Open (
                             RootDirectory,
                             &KernelFile,
                             (CHAR16 *)TryPath,
                             EFI_FILE_MODE_READ,
                             0
                             );

    for (PathIndex = 0; EFI_ERROR (Status) && FallbackPaths[PathIndex] != NULL; PathIndex++) {
      if (StrCmp (FallbackPaths[PathIndex], KernelPath) == 0) {
        continue;
      }
      KernelFile = NULL;
      Status = RootDirectory->Open (
                               RootDirectory,
                               &KernelFile,
                               (CHAR16 *)FallbackPaths[PathIndex],
                               EFI_FILE_MODE_READ,
                               0
                               );
      if (!EFI_ERROR (Status)) {
        DEBUG ((DEBUG_INFO, "DirectKernel: Found kernel at fallback path %s\n", FallbackPaths[PathIndex]));
      }
    }

    if (EFI_ERROR (Status)) {
      //
      // All direct paths failed — try APFS firmlink subvolume paths
      //
      {
        EFI_FILE_PROTOCOL  *SubvolDir;
        STATIC CONST CHAR16  *SubvolPaths[] = {
          L"\\System\\Volumes\\Shared Support\\com_apple_MobileAsset_MacSoftwareUpdate",
          L"\\System\\Volumes\\SharedSupport\\com_apple_MobileAsset_MacSoftwareUpdate",
          L"\\System\\Volumes\\iMAS\\com_apple_MobileAsset_MacSoftwareUpdate",
          NULL
        };

        for (UINTN Si = 0; SubvolPaths[Si] != NULL; Si++) {
          Status = RootDirectory->Open (RootDirectory, &SubvolDir, (CHAR16 *)SubvolPaths[Si], EFI_FILE_MODE_READ, 0);
          if (!EFI_ERROR (Status)) {
            UINTN   DirBufSize = SIZE_256KB;
            VOID    *DirBuf = AllocatePool (DirBufSize);
            if (DirBuf != NULL) {
              while (TRUE) {
                UINTN  ReadSz = DirBufSize;
                Status = SubvolDir->Read (SubvolDir, &ReadSz, DirBuf);
                if (EFI_ERROR (Status) || ReadSz == 0) {
                  break;
                }
                EFI_FILE_INFO  *Entry = (EFI_FILE_INFO *)DirBuf;
                while ((UINTN)Entry < (UINTN)DirBuf + ReadSz) {
                  if ((Entry->Attribute & EFI_FILE_DIRECTORY) == 0) {
                    UINTN  NameLen = StrLen (Entry->FileName);
                    if ((NameLen > 4) && (StrCmp (Entry->FileName + NameLen - 4, L".zip") == 0)) {
                      CHAR16  FullPath[512];
                      UnicodeSPrint (FullPath, sizeof (FullPath), L"%s\\%s", SubvolPaths[Si], Entry->FileName);
                      KernelBuffer = ReadKernelFromZip (RootDirectory, FullPath, L"kernelcache", &KernelSize);
                      if (KernelBuffer != NULL) {
                        AllocatedSize = KernelSize;
                        Is32Bit       = FALSE;
                        Status        = EFI_SUCCESS;
                        DEBUG ((DEBUG_INFO, "DirectKernel: Extracted kernel from firmlink: %s - %u bytes\n", FullPath, KernelSize));
                        SubvolDir->Close (SubvolDir);
                        FreePool (DirBuf);
                        goto SKIP_READ_APPLE_KERNEL;
                      }
                    }
                  }
                  if (Entry->Size == 0) {
                    break;
                  }
                  Entry = (EFI_FILE_INFO *)((UINT8 *)Entry + Entry->Size);
                }
              }
              FreePool (DirBuf);
            }
            SubvolDir->Close (SubvolDir);
            Status = EFI_NOT_FOUND;
          }
        }
      }

      //
      // All direct paths failed — try ZIP extraction
      //
      DEBUG ((DEBUG_INFO, "DirectKernel: Direct kernel open failed — searching ZIP files\n"));

      {
        STATIC CONST CHAR16  *ZipDirs[] = {
          L"\\com_apple_MobileAsset_MacSoftwareUpdate",
          L"\\System\\Volumes\\Shared Support\\com_apple_MobileAsset_MacSoftwareUpdate",
          L"\\System\\Volumes\\SharedSupport\\com_apple_MobileAsset_MacSoftwareUpdate",
          L"",
          NULL
        };

        for (UINTN Zdi = 0; !EFI_ERROR (Status) && ZipDirs[Zdi] != NULL; Zdi++) {
          EFI_FILE_PROTOCOL  *ZipDir;
          Status = RootDirectory->Open (
                                  RootDirectory,
                                  &ZipDir,
                                  (CHAR16 *)ZipDirs[Zdi],
                                  EFI_FILE_MODE_READ,
                                  0
                                  );
          if (!EFI_ERROR (Status)) {
            //
            // Enumerate directory for .zip files
            //
            UINTN   DirBufSize = SIZE_256KB;
            VOID    *DirBuf = AllocatePool (DirBufSize);
            if (DirBuf != NULL) {
              while (TRUE) {
                UINTN  ReadSz = DirBufSize;
                Status = ZipDir->Read (ZipDir, &ReadSz, DirBuf);
                if (EFI_ERROR (Status) || ReadSz == 0) {
                  break;
                }
                EFI_FILE_INFO  *Entry = (EFI_FILE_INFO *)DirBuf;
                while ((UINTN)Entry < (UINTN)DirBuf + ReadSz) {
                  if ((Entry->Attribute & EFI_FILE_DIRECTORY) == 0) {
                    UINTN  NameLen = StrLen (Entry->FileName);
                    if ((NameLen > 4) && (StrCmp (Entry->FileName + NameLen - 4, L".zip") == 0)) {
                      CHAR16  FullPath[512];
                      if (ZipDirs[Zdi][0] != L'\0') {
                        UnicodeSPrint (FullPath, sizeof (FullPath), L"%s\\%s", ZipDirs[Zdi], Entry->FileName);
                      } else {
                        UnicodeSPrint (FullPath, sizeof (FullPath), L"%s", Entry->FileName);
                      }

                      KernelBuffer = ReadKernelFromZip (RootDirectory, FullPath, L"kernelcache", &KernelSize);
                      if (KernelBuffer != NULL) {
                        AllocatedSize = KernelSize;
                        Is32Bit       = FALSE;
                        Status        = EFI_SUCCESS;
                        DEBUG ((DEBUG_INFO, "DirectKernel: Extracted kernel from %s: %u bytes\n", FullPath, KernelSize));
                        ZipDir->Close (ZipDir);
                        FreePool (DirBuf);
                        goto SKIP_READ_APPLE_KERNEL;
                      }
                    }
                  }
                  if (Entry->Size == 0) {
                    break;
                  }
                  Entry = (EFI_FILE_INFO *)((UINT8 *)Entry + Entry->Size);
                }
              }
              FreePool (DirBuf);
            }
            ZipDir->Close (ZipDir);
            Status = EFI_NOT_FOUND;
          }
        }
      }

      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "DirectKernel: Failed to open kernel - %r\n", Status));
        RootDirectory->Close (RootDirectory);
        return Status;
      }
    }
  }

SKIP_READ_APPLE_KERNEL:
  //
  // If kernel was extracted from ZIP, AllocatedSize > 0 and KernelBuffer is set.
  // Close KernelFile since we don't need it for ReadAppleKernel.
  // For the normal path (kernel from file), KernelFile stays open.
  //
  if (AllocatedSize > 0 && KernelBuffer != NULL && KernelFile != NULL) {
    KernelFile->Close (KernelFile);
    KernelFile = NULL;
  }

  if (EFI_ERROR (Status)) {
    RootDirectory->Close (RootDirectory);
    return Status;
  }

  //
  // If we got kernel from ZIP, skip ReadAppleKernel
  //
  if (AllocatedSize == 0 || KernelBuffer == NULL) {
    if (KernelFile == NULL) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: No kernel file handle available\n"));
      RootDirectory->Close (RootDirectory);
      return EFI_NOT_FOUND;
    }
    Status = ReadAppleKernel (
              KernelFile,
              FALSE,
              &Is32Bit,
              &KernelBuffer,
              &KernelSize,
              &AllocatedSize,
              0,
              NULL
              );

    if (KernelFile != NULL) {
      KernelFile->Close (KernelFile);
    }

    //
    // Re-host the kernel image in low memory.  ReadAppleKernel allocates
    // with AllocatePool, which lands in the 2.3-2.6 GB PCI-hole area on
    // this platform where reads are unreliable (observed: two different
    // values from the same address), corrupting the translated loads.
    //
    if (!EFI_ERROR (Status) && KernelBuffer != NULL &&
        (UINTN)KernelBuffer >= 0x40000000u) {
      VOID *LowBuf = AllocKernelImageBuffer ((UINTN)AllocatedSize);
      if (LowBuf != NULL) {
        CopyMem (LowBuf, KernelBuffer, (UINTN)KernelSize);
        FreePool (KernelBuffer);
        KernelBuffer = LowBuf;
        DEBUG ((DEBUG_INFO, "DirectKernel: re-hosted kernel buffer to 0x%llx\n",
                (UINT64)(UINTN)LowBuf));
      } else {
        DEBUG ((DEBUG_WARN, "DirectKernel: low-memory re-host failed, keeping 0x%llx\n",
                (UINT64)(UINTN)KernelBuffer));
      }
    }
  }

  RootDirectory->Close (RootDirectory);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DirectKernel: Failed to read kernel - %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DirectKernel: Loaded kernel %u bytes (%a)\n",
          KernelSize, Is32Bit ? "32-bit" : "64-bit"));

  ZeroMem (&MachoContext, sizeof (MachoContext));
  IsArm64 = IsArm64Kernel (KernelBuffer, KernelSize, FALSE);

  if (IsArm64) {
    //
    // ARM64 kernel — skip MachoInitializeContext (filters for x86_64).
    // Set up basics manually since we only need the entry point.
    //
    MACH_HEADER_64  *Hdr = (MACH_HEADER_64 *)KernelBuffer;

    //
    // Force the kernel console "enabled" flag to 1.  In the stock image it
    // is 0, which sends pe_kputc down the emergency slot path
    // (console slots 0x7E287B8 hold linker-signed pointers that cannot be
    // authenticated without the hardware PAC key).  With the flag set the
    // fast ret path is taken instead and the kernel boot continues without
    // stalling in a bogus console function.
    //
    if (KernelSize > 0x59BE860) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0x59BE860) = 1;
    }

    //
    // The timebase bit index global (0x7EB55C8) is written by the real
    // bootloader (boot.efi) before handing control to the kernel; in the
    // stock image it is 0xFFFFFFFF, which trips the 'invalid bit index'
    // panic in _enable_timebase_event_stream.  Zero is a valid index
    // (< 0x40), so patch it like the bootloader would.
    //
    if (KernelSize > 0xEB15C8) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0xEB15C8) = 0;   // simple offset
      if (KernelSize > 0xF235C8 + 4) {
        *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF235C8) = 0; // segment offset
      }
    }

    //
    // The kernel's NEON strlen (0xBB796F0) loops on LDR-Q/UMINV/FMOV,
    // which the DBT does not emulate: W2 never becomes zero, so the
    // CBZ exit (0xBB7972C) never fires and the loop spins forever.
    // Patch the loop exit to a NOP and the final FMOV to MOV W2,#0, so
    // the scan runs once and returns length 0 (bogus %s prints are
    // skipped) instead of hanging.
    //
    if (KernelSize > 0x4B7572C + 4) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0x4B7572C) = 0xD503201F;  // NOP (was cbnz w2, loop)
    }
    if (KernelSize > 0x4B75740 + 4) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0x4B75740) = 0x52800002;  // MOV W2, #0 (was fmov w2, s1)
    }

    //
    // The static-memory-ready flag [0x7F47C90] is 0 in the stock image
    // (set by the early boot memory setup that never runs here).  With it
    // clear, ml_static_ptovirt (0xC518580) returns error 1 and the kernel
    // panics 'kvtophys_nofail: VA->PA translation failed'.  Set the flag
    // so the static walk (and the AT S1E1R fallback, emulated as PA=VA in
    // the DBT) runs instead.
    //
    if (KernelSize > 0xFDDC90) {
      //
      // Set bit 0 of the static-memory-ready flag.  The word already
      // holds a meaningful value (0x01CEE164) that other code reads, so
      // OR the bit in instead of clobbering the whole word.  Also patch
      // the same flag as seen through the simple (base-offset) mapping
      // (0xF47C90) in case the translated load path resolves image
      // addresses that way.
      //
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0xFDDC90) |= 1;
      if (KernelSize > 0xF47C90) {
        *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF47C90) |= 1;
      }
    }

    //
    // The physical memory page holding 0x7F47C90..0x7F47CC0 is unreliable
    // on this platform (different reads of the same address return
    // different values, even in a freshly re-hosted low buffer).  The
    // static-memory walk can therefore never see the flag reliably.
    // Patch the code instead:
    //   - 0xC51858C 'tbz w8, #0' -> NOP, so ml_static_ptovirt always
    //     takes the main path (which falls through to the AT S1E1R
    //     fallback, emulated as PA=VA in the DBT)
    //   - [0x7F47C98] entry count -> 0, so the table scan is skipped
    //     (the file holds garbage 0x01CD56A0 that would loop ~30M times)
    //
    if (KernelSize > 0x551458C + 4) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0x551458C) = 0xD503201F;  // NOP (was tbz w8, #0)
    }
    if (KernelSize > 0xFDDC98 + 4) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0xFDDC98) = 0;            // entry count (segment)
      if (KernelSize > 0xF47C98 + 4) {
        *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF47C98) = 0;          // entry count (simple)
      }
    }

    //
    // Per-CPU slot table in the timebase continuation (0xBD53DA4): the
    // cpu-count global [0x7EB555C] and the slot-list pointer [0x7EB5578]
    // are 0 in the image (set by the early boot memory setup that never
    // runs here).  The translated loads resolve image addresses with the
    // simple base offset (KB + va - 0x7004000), so patch both the simple
    // and the segment-mapped offsets.  Point the list at a local slot
    // (0x7EB5560) whose mpidr-low16 = 0 matches the emulated MPIDR_EL1 = 0
    // and whose timer bit index is 0.
    //
    if (KernelSize > 0xF2755C) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF2755C) = 1;                    // cpu count (simple)
      if (KernelSize > 0xF27578 + 8) {
        *(volatile UINT64 *)((UINTN)KernelBuffer + 0xF27578) = 0xFFFFFE0007EB5560ull; // slot list (simple)
      }
      if (KernelSize > 0xF27574) {
        *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF27560) = 0;                  // slot[0] base (simple)
        *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF27564) = 0;                  // mpidr low16 (simple)
        *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF27574) = 0;                  // timer bit index (simple)
      }
      if (KernelSize > 0xF2355C) {
        *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF2355C) = 1;                  // cpu count (segment)
        if (KernelSize > 0xF23578 + 8) {
          *(volatile UINT64 *)((UINTN)KernelBuffer + 0xF23578) = 0xFFFFFE0007EB5560ull; // slot list (segment)
        }
        if (KernelSize > 0xF23574) {
          *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF23560) = 0;                // slot[0] base (segment)
          *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF23564) = 0;                // mpidr low16 (segment)
          *(volatile UINT32 *)((UINTN)KernelBuffer + 0xF23574) = 0;                // timer bit index (segment)
        }
      }
    }

    //
    // Data patches are unreliable on this platform (the __DATA_CONST
    // window maps onto memory holes that return 0 and drop writes), but
    // __TEXT_EXEC code patches work (NOP at 0xC51858C fixed the kvtophys
    // path).  In the per-CPU walk (0xBD53DA4) force a deterministic
    // found/not-found path that never forms garbage addresses:
    //   - 0xBD53DC0 'ldur x8,[x9,#0x1c]' -> 'add x8,x9,#4' so the slot
    //     base is the in-image address 0x7EB5560 (x9 = 0x7EB555C)
    //   - 0xBD53DCC 'cbz w9' -> 'b 0xBD53E18' so w10 = -1 (deterministic)
    // Then: x10 = 0x7EB54D8 (in image), timer bit index reads 0, LDSET
    // returns old 0 (0xCA61D08 file value 0) and the busy test passes.
    //
    if (KernelSize > 0x4D4FDC0 + 4) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0x4D4FDC0) = 0x91000528;  // add x8, x9, #4 (was ldur x8,[x9,#0x1c])
    }
    if (KernelSize > 0x4D4FDCC + 4) {
      *(volatile UINT32 *)((UINTN)KernelBuffer + 0x4D4FDCC) = 0x14000012;  // b 0xBD53E18 (was cbz w9)
    }
    if (Hdr->Signature != MACH_HEADER_64_SIGNATURE) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: Invalid Mach-O 64 magic %08X\n", Hdr->Signature));
      FreePool (KernelBuffer);
      return EFI_INVALID_PARAMETER;
    }
    //
    // Use a minimal fake context — the entry point code below handles LC_UNIXTHREAD directly.
    //
    MachoContext.MachHeader   = (MACH_HEADER_ANY *)Hdr;
    MachoContext.FileData     = KernelBuffer;
    MachoContext.FileSize     = KernelSize;
    MachoContext.InnerSize    = KernelSize;

    DEBUG ((DEBUG_INFO, "DirectKernel: ARM64 Mach-O parsed, NumCommands=%u\n", Hdr->NumCommands));
  } else {
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
    // For ARM64 kernels, extract entry point from thread state.
    // LC_UNIXTHREAD is cmd(4) + cmdsize(4) + flavor(4) + count(4) + state[],
    // where state is an arm_thread_state64_t: x0-x28, fp, lr, sp, pc, cpsr.
    // PC is UINT64 index 32 of the state.
    //
    Header64 = (MACH_HEADER_64 *)KernelBuffer;
    EntryPoint = 0;
    Cmd    = (MACH_LOAD_COMMAND *)((UINTN)Header64 + sizeof (MACH_HEADER_64));
    CmdEnd = (MACH_LOAD_COMMAND *)((UINTN)Cmd + Header64->CommandsSize);

    //
    // Walk all load commands: the LC_UNIXTHREAD entry point sits at the
    // start of the command list, but the LC_SEGMENT_64 layout needed for
    // VA->file-offset mapping only appears later in the list, so do not
    // stop at the thread state.
    //
    for (Index = 0; Index < Header64->NumCommands && Cmd < CmdEnd; ++Index) {
      if (Cmd->CommandType == MACH_LOAD_COMMAND_UNIX_THREAD) {
        if (EntryPoint == 0) {
          UINT32  *Triple;
          UINT32   Remaining;
          UINT32   Flavor;
          UINT32   Count;

          DEBUG ((DEBUG_INFO, "DirectKernel: Found LC_UNIXTHREAD at index %u\n", Index));
          //
          // LC_UNIXTHREAD: cmd(4) + cmdsize(4) + repeated triples of
          // flavor(4) + count(4) + state[count].  arm_thread_state64_t is
          // x0-x28, fp, lr, sp, pc, cpsr = 34 UINT64s, PC at UINT64 index 32,
          // so a full state needs 68 UINT32s.
          //
          Triple    = (UINT32 *)((UINTN)Cmd + 8);
          Remaining = (Cmd->CommandSize >= 8) ? Cmd->CommandSize - 8 : 0;
          while (Remaining >= 8) {
            Flavor = Triple[0];
            Count  = Triple[1];
            DEBUG ((DEBUG_INFO, "DirectKernel: LC_UNIXTHREAD flavor=%u count=%u\n", Flavor, Count));
            if (Flavor == ARM64_THREAD_STATE_FLAVOR && Count >= 66) {
              if ((UINTN)Triple + 8 + 34 * sizeof (UINT64) <= (UINTN)KernelBuffer + KernelSize) {
                UINT64  *ThrState = (UINT64 *)((UINTN)Triple + 8);

                EntryPoint = ThrState[32];
                DEBUG ((DEBUG_INFO, "DirectKernel: thr x0=0x%llx x1=0x%llx sp=0x%llx\n",
                        ThrState[0], ThrState[1], ThrState[31]));
                DEBUG ((DEBUG_INFO, "DirectKernel: thr pc=0x%llx cpsr=0x%llx\n",
                        ThrState[32], ThrState[33]));
              }
              break;
            }
            if (Remaining < 8 + Count * 4) {
              break;
            }
            Triple    += 2 + Count;
            Remaining -= 8 + Count * 4;
          }
        }
      } else if (Cmd->CommandType == MACH_LOAD_COMMAND_SEGMENT_64) {
        MACH_SEGMENT_COMMAND_64  *Seg64 = (MACH_SEGMENT_COMMAND_64 *)Cmd;

        if (SegCount < ARRAY_SIZE (SegVmAddr)) {
          SegVmAddr[SegCount]  = Seg64->VirtualAddress;
          SegVmSize[SegCount]  = Seg64->Size;
          SegFileOff[SegCount] = Seg64->FileOffset;
          DEBUG ((DEBUG_INFO, "DirectKernel: seg[%u] va=0x%llx sz=0x%llx off=0x%llx\n",
                  SegCount, Seg64->VirtualAddress, Seg64->Size, Seg64->FileOffset));
          SegCount++;
        } else {
          DEBUG ((DEBUG_WARN, "DirectKernel: segment table full (%u), ignoring\n", SegCount));
        }
      } else if (Cmd->CommandType == 0x80000028U) {  // LC_MAIN
        //
        // entry_point_command: cmd(4) + cmdsize(4) + entryoff(8) + stacksize(8)
        //
        if (EntryPoint == 0) {
          UINT64  *MainData = (UINT64 *)((UINTN)Cmd + 8);
          UINT64   FileOff  = MainData[0];
          DEBUG ((DEBUG_INFO, "DirectKernel: Found LC_MAIN at index %u fileoff=0x%llx\n", Index, FileOff));
          EntryPoint = (UINT64)(UINTN)KernelBuffer + FileOff;
          DEBUG ((DEBUG_INFO, "DirectKernel: LC_MAIN VM entry=0x%llx\n", EntryPoint));
        }
      }
      Cmd = (MACH_LOAD_COMMAND *)((UINTN)Cmd + Cmd->CommandSize);
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
  DEBUG ((DEBUG_VERBOSE, "DBT_LOADER: BootArgs init — Revision=%u Version=%u Mode=%llx\n",
          BootArgs->Revision, BootArgs->Version, BootArgs->efiMode));
  DEBUG ((DEBUG_VERBOSE, "DBT_LOADER: BootArgs — kaddr=0x%llx ksize=%u dtP=0x%llx dtLen=%u\n",
          BootArgs->kaddr, BootArgs->ksize, BootArgs->deviceTreeP, BootArgs->deviceTreeLength));
  DEBUG ((DEBUG_VERBOSE, "DBT_LOADER: CommandLine='%a'\n", BootArgs->CommandLine));

  BootArgs->Revision = kBootArgsRevision2_0;
  BootArgs->Version  = kBootArgsVersion2;
  BootArgs->efiMode  = Is32Bit ? kBootArgsEfiMode32 : kBootArgsEfiMode64;
  AsciiSPrint (BootArgs->CommandLine, BOOT_LINE_LENGTH, "install=1");

  BootArgs->kaddr = (UINT64)(UINTN)KernelBuffer;
  BootArgs->ksize  = KernelSize;

  DEBUG ((DEBUG_INFO, "DirectKernel: bootargs kaddr=0x%llx ksize=%u\n",
          BootArgs->kaddr, BootArgs->ksize));

  //
  // Create minimal device tree for XNU
  //
  Status = CreateMinimalDeviceTree (&DeviceTreeBuffer, &DeviceTreeSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "DirectKernel: Failed to create device tree - %r\n", Status));
    DeviceTreeBuffer = NULL;
    DeviceTreeSize = 0;
  } else {
    BootArgs->deviceTreeP = (UINT64)(UINTN)DeviceTreeBuffer;
    BootArgs->deviceTreeLength = (UINT32)DeviceTreeSize;
    DEBUG ((DEBUG_INFO, "DirectKernel: devicetree=0x%llx/%u\n",
            BootArgs->deviceTreeP, BootArgs->deviceTreeLength));
  }

  //
  // Allocate stack for kernel execution
  //
  StackSize = EFI_PAGES_TO_SIZE (0x100);  // 1MB stack
  Status = gBS->AllocatePool (EfiBootServicesData, StackSize, (VOID **)&StackBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DirectKernel: Failed to allocate stack - %r\n", Status));
    if (DeviceTreeBuffer != NULL) {
      FreePool (DeviceTreeBuffer);
    }
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_OUT_OF_RESOURCES;
  }
  DEBUG ((DEBUG_INFO, "DirectKernel: stack=%p size=%u\n", StackBuffer, StackSize));

  //
  // For ARM64 kernel, use DBT translation
  //
  if (IsArm64 && gDbtContext != NULL) {
    DBT_ARM64_STATE  ArmContext;
    UINT64           KernelVaBase = 0;
    UINT64           KernelVaEnd  = 0;
    UINTN            Steps;
    UINTN            MaxSteps;

    ZeroMem (&ArmContext, sizeof (ArmContext));
    ArmContext.X[0] = (UINT64)(UINTN)BootArgs;
    //
    // xnu's _start (osfmk/arm64/start.s) copies x3 into x20 before using the
    // boot args, so hand the pointer over in both registers.
    //
    ArmContext.X[3] = (UINT64)(UINTN)BootArgs;
    ArmContext.SP   = (UINT64)(UINTN)(StackBuffer + StackSize);
    ArmContext.PC   = EntryPoint;
    ArmContext.SPSR_EL1 = 0x5;  // EL1 with all exceptions masked

    DEBUG ((DEBUG_INFO, "DirectKernel: SP=0x%llx, PC=0x%llx\n", ArmContext.SP, ArmContext.PC));

    //
    // Dispatch loop: translate the straight-line block ending at the next
    // PC-updating branch, execute it, and continue from the resulting PC.
    // The kernel image is linked at the lowest segment vmaddr and loaded at
    // KernelBuffer; the entry point (LC_UNIXTHREAD pc) points at the start
    // of the entry segment (e.g. __TEXT_BOOT_EXEC), which lives at a
    // nonzero file offset, so VA->file-offset mapping goes through the
    // LC_SEGMENT_64 table rather than assuming entry == image base.
    //
    DEBUG ((DEBUG_VERBOSE, "DBT_LOADER: MachO Segments=%u Base=0x%llx End=0x%llx\n",
          SegCount, KernelVaBase, KernelVaEnd));

  KernelVaBase = EntryPoint;
    for (Index = 0; Index < SegCount; Index++) {
      if (SegVmAddr[Index] < KernelVaBase) {
        KernelVaBase = SegVmAddr[Index];
      }
    }
    KernelVaEnd  = KernelVaBase + KernelSize;
    MaxSteps     = 0x4000000;  // safety valve

    DEBUG ((DEBUG_INFO, "DirectKernel: %u segments, image base=0x%llx end=0x%llx\n",
            SegCount, KernelVaBase, KernelVaEnd));

    //
    // Register the segment table with the DBT context so translated loads
    // and stores translate guest VAs to host addresses.  Without this the
    // emitters would dereference the guest VA directly on the x86 host and
    // the silently swallowed page fault would leave guest registers zeroed
    // (observed as a CBZ self-spin on the kernel boot path).
    //
    if (EFI_ERROR (DbtSetSegments (gDbtContext, SegCount, SegVmAddr, SegVmSize,
                                   SegFileOff, KernelBuffer))) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: DbtSetSegments failed\n"));
      FreePool (StackBuffer);
      FreePool (BootArgs);
      FreePool (KernelBuffer);
      return EFI_INVALID_PARAMETER;
    }

    //
    // Register a host-backed zeroed window for low guest "physical" VAs.
    // The kernel's early boot shim touches globals at small offsets (e.g.
    // 0x1C0); with no backing they identity-map onto the x86 firmware's own
    // low pages and the read yields unrelated bytes the kernel later takes
    // for a pointer (observed as a hang after the CPU-scan handoff).  The
    // kernel's early console ring also lives by pointer at ~0x5967000 /
    // ~0x5A7C000 (~93-95MB, static __DATA globals), so cover the whole
    // 256MB.
    //
    {
      UINTN  PhysWinSize = EFI_PAGES_TO_SIZE (65536);  // 256MB
      UINT8 *PhysWinBuffer;
      Status = gBS->AllocatePages (
                      AllocateAnyPages,
                      EfiLoaderData,
                      EFI_SIZE_TO_PAGES (PhysWinSize),
                      (EFI_PHYSICAL_ADDRESS *)&PhysWinBuffer
                      );
      if (!EFI_ERROR (Status)) {
        ZeroMem (PhysWinBuffer, PhysWinSize);
        DEBUG ((DEBUG_INFO, "DirectKernel: phys window allocated host=%p sz=0x%x\n",
                PhysWinBuffer, PhysWinSize));

        if (EFI_ERROR (DbtSetPhysWindow (gDbtContext, 0, PhysWinSize, PhysWinBuffer))) {
          DEBUG ((DEBUG_ERROR, "DirectKernel: DbtSetPhysWindow failed\n"));
          gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)PhysWinBuffer,
                          EFI_SIZE_TO_PAGES (PhysWinSize));
        }
      } else {
        DEBUG ((DEBUG_WARN, "DirectKernel: phys window alloc failed - %r\n", Status));
      }
    }

    //
    // Host-backed staging window for the kernel's high "physical" handles.
    // The boot code reads/dereferences a DRAM pointer at 0x4000000000
    // (observed as 0x4000020000) to locate its early physical region; when
    // no segment maps it DBT identity-maps the VA straight onto the x86
    // firmware address space where it is inaccessible and the boot hangs
    // (the same class of failure as the low 0x1C0 window fixed earlier).
    // Allocate a zeroed staging buffer at that handle so the kernel has a
    // deterministic region to poke at while it builds its own tables.
    //
    {
      UINT64 DramWinBase = 0x4000000000ULL;
      UINTN  DramWinSize = EFI_PAGES_TO_SIZE (1024);  // 4MB staging
      UINT8 *DramWinBuffer;
      Status = gBS->AllocatePages (
                      AllocateAnyPages,
                      EfiLoaderData,
                      EFI_SIZE_TO_PAGES (DramWinSize),
                      (EFI_PHYSICAL_ADDRESS *)&DramWinBuffer
                      );
      if (!EFI_ERROR (Status)) {
        ZeroMem (DramWinBuffer, DramWinSize);
        DEBUG ((DEBUG_INFO, "DirectKernel: dram window allocated host=%p base=0x%llx sz=0x%x\n",
                DramWinBuffer, DramWinBase, DramWinSize));

        if (EFI_ERROR (DbtSetPhysWindow (gDbtContext, DramWinBase, DramWinSize, DramWinBuffer))) {
          DEBUG ((DEBUG_ERROR, "DirectKernel: dram window set failed\n"));
          gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)DramWinBuffer,
                          EFI_SIZE_TO_PAGES (DramWinSize));
        }
      } else {
        DEBUG ((DEBUG_WARN, "DirectKernel: dram staging alloc failed - %r\n", Status));
      }
    }

    //
    // Host-backed window for the kernel's static per-CPU slot table region.
    // The image carries a __DATA global pointing into low physical space
    // (observed base 0x10000005ABDE40, 16KB stride per CPU) that boot code
    // atomically pokes via cpu_number<<14 offsets.  With no backing the DBT
    // identity-maps it onto a host address that does not exist and the
    // LDADD dies on the first counter increment (log ended exactly at such
    // an access, PC 0xFFFFFE000C361B54).  Back it with a zeroed buffer.
    //
    {
      UINT64 CpuSlotWinBase = 0x10000005ABD000ULL;  // page-aligned below 0x...5ABDE40
      UINTN  CpuSlotWinSize = EFI_PAGES_TO_SIZE (128);  // 512KB staging
      UINT8 *CpuSlotWinBuffer;
      Status = gBS->AllocatePages (
                      AllocateAnyPages,
                      EfiLoaderData,
                      EFI_SIZE_TO_PAGES (CpuSlotWinSize),
                      (EFI_PHYSICAL_ADDRESS *)&CpuSlotWinBuffer
                      );
      if (!EFI_ERROR (Status)) {
        ZeroMem (CpuSlotWinBuffer, CpuSlotWinSize);
        DEBUG ((DEBUG_INFO, "DirectKernel: cpu slot window host=%p base=0x%llx sz=%x VerboseStep=%u\n",
                CpuSlotWinBuffer, CpuSlotWinBase, CpuSlotWinSize, VerboseStep));

        if (EFI_ERROR (DbtSetPhysWindow (gDbtContext, CpuSlotWinBase, CpuSlotWinSize, CpuSlotWinBuffer))) {
          DEBUG ((DEBUG_ERROR, "DirectKernel: cpu slot window set failed\n"));
          gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)CpuSlotWinBuffer,
                          EFI_SIZE_TO_PAGES (CpuSlotWinSize));
        }
      } else {
        DEBUG ((DEBUG_WARN, "DirectKernel: cpu slot window alloc failed - %r\n", Status));
      }
    }

    //
    // Populate the per-CPU data entries handoff table that xnu's boot code
    // scans (iBoot would normally fill it); without this the kernel reads
    // stale file data and faults dereferencing it.
    //
    if (SetupCpuDataEntries (KernelBuffer, SegCount, SegVmAddr, SegVmSize,
                             SegFileOff, EntryPoint, StackBuffer, StackSize) != NULL) {
      //
      // TPIDR_EL1 is the kernel's per-CPU pointer (it reads cpu_number at
      // [tpidr_el1,#0x1b8] to index its per-CPU slot array).  Point it at the
      // fake cpu_data we planted inside __TEXT_BOOT_EXEC so the slot-array
      // base is the static value the image carries, not 0 (which previously
      // sent the dereference into the low phys window).
      //
      ArmContext.TPIDR_EL1 = (UINT64)EntryPoint + 0x77B;
      DEBUG ((DEBUG_INFO, "DirectKernel: TPIDR_EL1 -> cpu_data va=0x%llx\n", ArmContext.TPIDR_EL1));
    }

    DEBUG ((DEBUG_INFO, "DirectKernel: dispatch start pc=0x%llx maxsteps=%u\n",
            ArmContext.PC, MaxSteps));

    for (Steps = 0;
         Steps < MaxSteps &&
         ArmContext.PC >= KernelVaBase &&
         ArmContext.PC <  KernelVaEnd;
         Steps++) {
      UINTN   Pa       = 0;
      UINTN   FileOff  = 0;
      BOOLEAN Mapped   = FALSE;
      UINT32  Inst;
      UINTN   Off;

      for (Index = 0; Index < SegCount; Index++) {
        if (ArmContext.PC >= SegVmAddr[Index] &&
            ArmContext.PC <  SegVmAddr[Index] + SegVmSize[Index]) {
          FileOff = (UINTN)(SegFileOff[Index] + (ArmContext.PC - SegVmAddr[Index]));
          Mapped  = TRUE;
          break;
        }
      }

      if (Mapped) {
        Pa = (UINTN)((UINT8 *)KernelBuffer + FileOff);
      } else if (SegCount == 0) {
        // No segment table (legacy image): fall back to a contiguous mapping.
        Pa = (UINTN)((UINT8 *)KernelBuffer + (UINTN)(ArmContext.PC - KernelVaBase));
      } else {
        DEBUG ((DEBUG_INFO, "DirectKernel: kernel left image range at PC=0x%llx after %u steps\n",
                ArmContext.PC, Steps));
        DEBUG ((DEBUG_INFO, "DirectKernel: PC=0x%llx SP=0x%llx LR=0x%llx PST=0x%llx\n",
                ArmContext.PC, ArmContext.SP, ArmContext.X[30], ArmContext.PSTATE));
        DEBUG ((DEBUG_INFO, "DirectKernel: x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
                ArmContext.X[0], ArmContext.X[1], ArmContext.X[2], ArmContext.X[3]));
        DEBUG ((DEBUG_INFO, "DirectKernel: x8=0x%llx x9=0x%llx x19=0x%llx x20=0x%llx\n",
                ArmContext.X[8], ArmContext.X[9], ArmContext.X[19], ArmContext.X[20]));
        DEBUG ((DEBUG_INFO, "DirectKernel: x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx\n",
                ArmContext.X[21], ArmContext.X[22], ArmContext.X[23], ArmContext.X[24]));
        DEBUG ((DEBUG_INFO, "DirectKernel: x25=0x%llx x26=0x%llx x27=0x%llx x28=0x%llx\n",
                ArmContext.X[25], ArmContext.X[26], ArmContext.X[27], ArmContext.X[28]));
        break;
      }

      if (FileOff >= KernelSize) {
        DEBUG ((DEBUG_INFO, "DirectKernel: kernel ran off the image at PC=0x%llx\n", ArmContext.PC));
        break;
      }

      Off = 0;
      for (;;) {
        if (Off + 4 > KernelSize - FileOff) {
          break;
        }
        Inst = *(UINT32 *)(Pa + Off);
        if (IsPcUpdatingBranch (Inst)) {
          break;
        }
        Off += 4;
      }

      if (Off + 4 > KernelSize - FileOff) {
        DEBUG ((DEBUG_INFO, "DirectKernel: kernel ran off the image at PC=0x%llx\n", ArmContext.PC));
        break;
      }

      DbtTranslateBlock (gDbtContext, (VOID *)(Pa), Off + 4, ArmContext.PC, NULL);
      DbtExecute (gDbtContext, &ArmContext);
    }

    if (ArmContext.PC >= KernelVaBase && ArmContext.PC < KernelVaEnd) {
      DEBUG ((DEBUG_WARN, "DirectKernel: dispatcher step limit reached at PC=0x%llx\n", ArmContext.PC));
    } else {
      DEBUG ((DEBUG_INFO, "DirectKernel: kernel left image range at PC=0x%llx after %u steps\n",
              ArmContext.PC, Steps));
      DEBUG ((DEBUG_INFO, "DirectKernel: PC=0x%llx SP=0x%llx LR=0x%llx PST=0x%llx\n",
              ArmContext.PC, ArmContext.SP, ArmContext.X[30], ArmContext.PSTATE));
      DEBUG ((DEBUG_INFO, "DirectKernel: x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
              ArmContext.X[0], ArmContext.X[1], ArmContext.X[2], ArmContext.X[3]));
      DEBUG ((DEBUG_INFO, "DirectKernel: x8=0x%llx x9=0x%llx x19=0x%llx x20=0x%llx\n",
              ArmContext.X[8], ArmContext.X[9], ArmContext.X[19], ArmContext.X[20]));
      DEBUG ((DEBUG_INFO, "DirectKernel: x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx\n",
              ArmContext.X[21], ArmContext.X[22], ArmContext.X[23], ArmContext.X[24]));
      DEBUG ((DEBUG_INFO, "DirectKernel: x25=0x%llx x26=0x%llx x27=0x%llx x28=0x%llx\n",
              ArmContext.X[25], ArmContext.X[26], ArmContext.X[27], ArmContext.X[28]));
      DEBUG ((DEBUG_VERBOSE, "DBT_LOADER: Final state Steps=%u PC=0x%llx SP=0x%llx\n",
              Steps, ArmContext.PC, ArmContext.SP));
    }

    // Should not reach here
    FreePool (StackBuffer);
    if (DeviceTreeBuffer != NULL) {
      FreePool (DeviceTreeBuffer);
    }
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_DEVICE_ERROR;
#if defined(__x86_64__) || defined(_M_X64)
  } else if (!IsArm64) {
    //
    // x86_64 kernel — native direct boot on the x64 host.
    //
    // The kernel cache is PRELINKED (has __PRELINK_INFO), so no kext injection
    // is needed. We re-host the image below 4 GB, build fresh page tables
    // (identity RAM + segment VAs at slide 0), fill BootArgs2 with the EFI
    // memory map and a < 4 GB EFI System Table copy, ExitBootServices and
    // jump to the LC_UNIXTHREAD entry point with the boot args pointer in
    // %rsi/%rdi/%rdx.
    //
    UINT64                 KernelPhys;
    UINT8                  *NewBuffer;
    UINT8                  *NewDt;
    UINT8                  *BssBuffer;
    UINT64                 BssTotal;
    UINT64                 BssCursor;
    UINT64                 VaBase;
    UINT64                 VaEnd;
    UINT64                 Cr3;
    UINT64                 TablePhys;
    UINT64                 DtPhys;
    UINTN                  TablesPages;
    UINT8                  *MemMapBuffer;
    UINTN                  MemMapSize;
    UINTN                  MemMapDescSize;
    UINTN                  MemMapDescVersion;
    UINT64                 PhysicalMemorySize;
    UINT64                 SegFileSz[32];
    MACH_HEADER_64         *KHeader;
    MACH_LOAD_COMMAND      *KCmd;
    MACH_LOAD_COMMAND      *KCmdEnd;
    UINTN                  KIdx;
    UINTN                  SegIdx;
    UINTN                  PageIdx;
    UINTN                  MapIdx;
    UINTN                  Attempt;
    EFI_STATUS             ExitStatus;
    UINTN                  MapKey;
    DBT_X86_PAGE_TABLE     PageTable;

    KernelPhys   = 0;
    NewBuffer    = NULL;
    NewDt        = NULL;
    BssBuffer    = NULL;
    BssTotal     = 0;
    BssCursor    = 0;
    VaBase       = 0;
    VaEnd        = 0;
    Cr3          = 0;
    TablePhys    = 0;
    DtPhys       = 0;
    TablesPages  = 0;
    MemMapBuffer = NULL;
    MemMapSize   = 0;
    MemMapDescSize = 0;
    MemMapDescVersion = 0;
    PhysicalMemorySize = 0;
    ZeroMem (SegFileSz, sizeof (SegFileSz));

    //
    // Walk the load commands: fill the segment table (VA / mem size /
    // file offset / file size) and compute the linked VA span plus the
    // total BSS slack (mem size beyond file size).
    //
    KHeader = (MACH_HEADER_64 *)KernelBuffer;
    KCmd    = (MACH_LOAD_COMMAND *)((UINTN)KHeader + sizeof (MACH_HEADER_64));
    KCmdEnd = (MACH_LOAD_COMMAND *)((UINTN)KCmd + KHeader->CommandsSize);
    VaBase  = ~((UINT64)0);
    VaEnd   = 0;

    DEBUG ((DEBUG_INFO, "DirectKernel: x86_64 kernel cputype=%u ncmds=%u\n",
            (UINT32)KHeader->CpuType, (UINT32)KHeader->NumCommands));

    for (KIdx = 0; KIdx < KHeader->NumCommands && KCmd < KCmdEnd; ++KIdx) {
      if (KCmd->CommandType == MACH_LOAD_COMMAND_SEGMENT_64) {
        MACH_SEGMENT_COMMAND_64  *Seg = (MACH_SEGMENT_COMMAND_64 *)KCmd;

        if (SegCount < ARRAY_SIZE (SegVmAddr)) {
          SegVmAddr[SegCount]  = Seg->VirtualAddress;
          SegVmSize[SegCount]  = Seg->Size;
          SegFileOff[SegCount] = Seg->FileOffset;
          SegFileSz[SegCount]  = Seg->FileSize;

          if (Seg->Size > Seg->FileSize) {
            BssTotal += Seg->Size - Seg->FileSize;
          }

          if (Seg->Size > 0) {
            if (Seg->VirtualAddress < VaBase) {
              VaBase = Seg->VirtualAddress;
            }
            if (Seg->VirtualAddress + Seg->Size > VaEnd) {
              VaEnd = Seg->VirtualAddress + Seg->Size;
            }
          }

          DEBUG ((DEBUG_VERBOSE, "DirectKernel: x86 seg[%u] va=0x%llx sz=0x%llx off=0x%llx fsz=0x%llx\n",
                  SegCount, Seg->VirtualAddress, Seg->Size, Seg->FileOffset, Seg->FileSize));
          SegCount++;
        } else {
          DEBUG ((DEBUG_WARN, "DirectKernel: x86 segment table full, ignoring\n"));
        }
      }
      KCmd = (MACH_LOAD_COMMAND *)((UINTN)KCmd + KCmd->CommandSize);
    }

    if (SegCount == 0 || VaBase >= VaEnd) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: no usable LC_SEGMENT_64 table\n"));
      FreePool (StackBuffer);
      FreePool (BootArgs);
      FreePool (KernelBuffer);
      return EFI_INVALID_PARAMETER;
    }

    DEBUG ((DEBUG_INFO, "DirectKernel: x86 %u segments va=[0x%llx..0x%llx) bss=0x%llx\n",
            SegCount, VaBase, VaEnd, BssTotal));

    //
    // Re-host the kernel image below 4 GB, page aligned. BootArgs2.kaddr is
    // UINT32 on x86.
    //
    ExitStatus = X86AllocLow (EFI_SIZE_TO_PAGES ((UINTN)KernelSize), &NewBuffer);
    if (EFI_ERROR (ExitStatus)) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: cannot re-host kernel below 4GB - %r\n", ExitStatus));
      FreePool (StackBuffer);
      FreePool (BootArgs);
      FreePool (KernelBuffer);
      return ExitStatus;
    }
    CopyMem (NewBuffer, KernelBuffer, (UINTN)KernelSize);
    FreePool (KernelBuffer);
    KernelBuffer = NewBuffer;
    KernelPhys   = (UINT64)(UINTN)KernelBuffer;
    DEBUG ((DEBUG_INFO, "DirectKernel: kernel re-hosted at 0x%llx (%u bytes)\n", KernelPhys, KernelSize));

    //
    // Re-host the minimal device tree below 4 GB (BootArgs2.deviceTreeP is
    // UINT32 and xnu reads the tree by physical address). Note the copy is
    // performed before the memory-map capture, so the copied range shows up
    // as resident loader data in the handed-over boot map.
    //
    if (DeviceTreeBuffer != NULL && DeviceTreeSize > 0) {
      ExitStatus = X86AllocLow (EFI_SIZE_TO_PAGES ((UINTN)DeviceTreeSize), &NewDt);
      if (EFI_ERROR (ExitStatus)) {
        DEBUG ((DEBUG_WARN, "DirectKernel: device tree re-host failed - %r\n", ExitStatus));
        NewDt = NULL;
        DeviceTreeSize = 0;
      } else {
        CopyMem (NewDt, DeviceTreeBuffer, (UINTN)DeviceTreeSize);
        DtPhys = (UINT64)(UINTN)NewDt;
        DEBUG ((DEBUG_INFO, "DirectKernel: device tree re-hosted at 0x%llx (%u bytes)\n", DtPhys, DeviceTreeSize));
      }
    }

    //
    // Fresh page tables.
    //
    ZeroMem (&PageTable, sizeof (PageTable));
    ExitStatus = gBS->AllocatePages (
                        AllocateAnyPages,
                        EfiReservedMemoryType,
                        1,
                        (EFI_PHYSICAL_ADDRESS *)&PageTable.Pml4
                        );
    if (EFI_ERROR (ExitStatus)) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: PML4 alloc failed\n"));
      FreePool (StackBuffer);
      FreePool (BootArgs);
      FreePool (KernelBuffer);
      return ExitStatus;
    }
    ZeroMem ((VOID *)(UINTN)PageTable.Pml4, EFI_PAGE_SIZE);

    //
    // Zeroed backing for every BSS page (mem size beyond file size).
    //
    if (BssTotal > 0) {
      EFI_PHYSICAL_ADDRESS  BssAddr;

      BssAddr = 0x100000000ULL;
      ExitStatus = gBS->AllocatePages (
                          AllocateMaxAddress,
                          EfiReservedMemoryType,
                          EFI_SIZE_TO_PAGES ((UINTN)BssTotal),
                          &BssAddr
                          );
      if (EFI_ERROR (ExitStatus)) {
        DEBUG ((DEBUG_ERROR, "DirectKernel: BSS backing alloc failed\n"));
        gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)PageTable.Pml4, 1);
        FreePool (StackBuffer);
        FreePool (BootArgs);
        FreePool (KernelBuffer);
        return ExitStatus;
      }
      BssBuffer = (UINT8 *)(UINTN)BssAddr;
      ZeroMem (BssBuffer, (UINTN)BssTotal);
    }

    //
    // < 4 GB copy of the EFI System Table for BootArgs2.efiSystemTable.
    //
    TablesPages = EFI_SIZE_TO_PAGES (
                    sizeof (EFI_SYSTEM_TABLE) +
                    sizeof (EFI_RUNTIME_SERVICES) +
                    gST->NumberOfTableEntries * sizeof (EFI_CONFIGURATION_TABLE) +
                    EFI_PAGE_SIZE);
    ExitStatus = X86CopyEfiSystemTable (&TablePhys);
    if (EFI_ERROR (ExitStatus)) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: EFI System Table copy failed\n"));
      if (BssBuffer != NULL) {
        gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)BssBuffer, EFI_SIZE_TO_PAGES ((UINTN)BssTotal));
      }
      gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)PageTable.Pml4, 1);
      FreePool (StackBuffer);
      FreePool (BootArgs);
      FreePool (KernelBuffer);
      return ExitStatus;
    }

    //
    // Capture the EFI memory map (reserved, < 4 GB, page aligned).
    //
    ExitStatus = X86CaptureMemoryMap (
                   &MemMapBuffer,
                   &MemMapSize,
                   &MemMapDescSize,
                   &MemMapDescVersion,
                   &PhysicalMemorySize
                   );
    if (EFI_ERROR (ExitStatus)) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: memory map capture failed\n"));
      gBS->FreePages ((EFI_PHYSICAL_ADDRESS)TablePhys, TablesPages);
      if (BssBuffer != NULL) {
        gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)BssBuffer, EFI_SIZE_TO_PAGES ((UINTN)BssTotal));
      }
      gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)PageTable.Pml4, 1);
      FreePool (StackBuffer);
      FreePool (BootArgs);
      FreePool (KernelBuffer);
      return ExitStatus;
    }
    DEBUG ((DEBUG_INFO, "DirectKernel: memory map at 0x%llx size=%u phys=%llu\n",
            (UINT64)(UINTN)MemMapBuffer, MemMapSize, PhysicalMemorySize));

    //
    // Identity-map every memory-map range (2 MB pages). This covers the
    // kernel image, page tables, boot args, the handoff stack, UEFI runtime
    // services and any framebuffer above 4 GB.
    //
    for (MapIdx = 0; MapIdx < MemMapSize; MapIdx += MemMapDescSize) {
      EFI_MEMORY_DESCRIPTOR  *Desc = (EFI_MEMORY_DESCRIPTOR *)(MemMapBuffer + MapIdx);

      if (Desc->NumberOfPages > 1) {
        UINT64  RangeEnd = Desc->PhysicalStart +
                           (UINT64)Desc->NumberOfPages * 4096ULL;

        if (!X86PtIdentityRange (&PageTable, Desc->PhysicalStart, RangeEnd)) {
          DEBUG ((DEBUG_ERROR, "DirectKernel: identity map failed at 0x%llx\n", Desc->PhysicalStart));
          break;
        }
      }
    }

    //
    // Map each kernel segment VA range at slide 0 to its host address.
    // Pages beyond the file size (BSS) map into the zeroed backing.
    //
    for (SegIdx = 0; SegIdx < SegCount; ++SegIdx) {
      UINT64  SegVa     = SegVmAddr[SegIdx];
      UINT64  SegSize   = SegVmSize[SegIdx];
      UINT64  SegFile   = SegFileOff[SegIdx];
      UINT64  SegFDSize = SegFileSz[SegIdx];
      UINTN   PageCount = (UINTN)((SegSize + 0xFFF) >> 12);

      for (PageIdx = 0; PageIdx < PageCount; ++PageIdx) {
        UINT64  Va;
        UINT64  FileDelta;
        UINT64  Pa;

        Va        = SegVa + PageIdx * 0x1000ULL;
        FileDelta = SegFile + PageIdx * 0x1000ULL;

        if (FileDelta < SegFDSize) {
          Pa = KernelPhys + FileDelta;
        } else {
          Pa = (UINT64)(UINTN)BssBuffer + BssCursor;
          BssCursor += 0x1000ULL;
        }

        if (!X86PtMap4K (&PageTable, Va, Pa)) {
          DEBUG ((DEBUG_ERROR, "DirectKernel: kernel VA map failed at 0x%llx\n", Va));
          BssTotal = 0;
          goto X86Cleanup;
        }
      }
    }
    DEBUG ((DEBUG_INFO, "DirectKernel: x86 kernel VAs mapped (slide=0)\n"));

    //
    // Fill the remaining BootArgs2 fields for the native x86 boot.
    //
    BootArgs->kslide                = 0;
    BootArgs->kaddr                 = (UINT32)KernelPhys;
    BootArgs->ksize                 = (UINT32)(KernelSize + BssTotal);
    if (DtPhys != 0) {
      BootArgs->deviceTreeP         = (UINT32)DtPhys;
      BootArgs->deviceTreeLength    = (UINT32)DeviceTreeSize;
    }
    BootArgs->KC_hdrs_vaddr         = VaBase;
    BootArgs->MemoryMap             = (UINT32)(UINTN)MemMapBuffer;
    BootArgs->MemoryMapSize         = (UINT32)MemMapSize;
    BootArgs->MemoryMapDescriptorSize    = (UINT32)MemMapDescSize;
    BootArgs->MemoryMapDescriptorVersion = (UINT32)MemMapDescVersion;
    BootArgs->efiSystemTable        = (UINT32)TablePhys;
    BootArgs->efiRuntimeServicesPageStart = (UINT32)(TablePhys >> 12);
    BootArgs->efiRuntimeServicesPageCount = (UINT32)TablesPages;
    BootArgs->efiRuntimeServicesVirtualPageStart = TablePhys;
    BootArgs->PhysicalMemorySize    = PhysicalMemorySize;
    BootArgs->FSBFrequency          = 100000000ULL;   /* 100 MHz nominal */
    BootArgs->pciConfigSpaceBaseAddress = 0xE0000000ULL;
    BootArgs->pciConfigSpaceStartBusNumber = 0;
    BootArgs->pciConfigSpaceEndBusNumber = 0xFF;
    BootArgs->csrActiveConfig       = 0;
    BootArgs->csrCapabilities       = 0;
    BootArgs->flags |= kBootArgsFlagInstallUI;
    BootArgs->debugMode = 0;
    AsciiSPrint (BootArgs->CommandLine, BOOT_LINE_LENGTH, "install=1 -v");

    Cr3 = (UINT64)(UINTN)PageTable.Pml4;
    DEBUG ((DEBUG_INFO, "DirectKernel: x86 boot ready cr3=0x%llx entry=0x%llx args=0x%llx\n",
            Cr3, EntryPoint, (UINT64)(UINTN)BootArgs));

    //
    // Capture the map key, then leave boot services. ExitBootServices can
    // report a stale key if the map changed since; retry a few times by
    // recapturing into the same reserved buffer.
    //
    for (Attempt = 0; Attempt < 8; ++Attempt) {
      UINTN  RetrySize;
      UINTN  RetryDescSize;
      UINT32 RetryDescVersion;

      RetrySize       = MemMapSize;
      RetryDescSize   = 0;
      RetryDescVersion = 0;
      ExitStatus = gBS->GetMemoryMap (
                          &RetrySize,
                          (EFI_MEMORY_DESCRIPTOR *)MemMapBuffer,
                          &MapKey,
                          &RetryDescSize,
                          &RetryDescVersion
                          );
      if (EFI_ERROR (ExitStatus)) {
        break;
      }

      BootArgs->MemoryMap                  = (UINT32)(UINTN)MemMapBuffer;
      BootArgs->MemoryMapSize              = (UINT32)RetrySize;
      BootArgs->MemoryMapDescriptorSize    = (UINT32)RetryDescSize;
      BootArgs->MemoryMapDescriptorVersion = (UINT32)RetryDescVersion;

      DEBUG ((DEBUG_INFO, "DirectKernel: ExitBootServices attempt %u\n", Attempt));
      ExitStatus = gBS->ExitBootServices (gX86ImageHandle, MapKey);
      if (!EFI_ERROR (ExitStatus)) {
        break;
      }
    }

    if (EFI_ERROR (ExitStatus)) {
      DEBUG ((DEBUG_ERROR, "DirectKernel: ExitBootServices failed - %r\n", ExitStatus));
      goto X86Cleanup;
    }

    DEBUG ((DEBUG_INFO, "DirectKernel: jumping to xnu x86_64 entry...\n"));
    X86JumpKernel (Cr3, EntryPoint, (UINT64)(UINTN)BootArgs);

    //
    // Unreachable: the kernel never returns.
    //
    X86Cleanup:
    DEBUG ((DEBUG_ERROR, "DirectKernel: x86 native boot aborted\n"));
    if (TablePhys != 0) {
      gBS->FreePages ((EFI_PHYSICAL_ADDRESS)TablePhys, TablesPages);
    }
    if (BssBuffer != NULL && BssTotal > 0) {
      gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)BssBuffer, EFI_SIZE_TO_PAGES ((UINTN)BssTotal));
    }
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)PageTable.Pml4, 1);
    if (DeviceTreeBuffer != NULL) {
      FreePool (DeviceTreeBuffer);
    }
    FreePool (StackBuffer);
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_DEVICE_ERROR;
#else
    DEBUG ((DEBUG_ERROR, "DirectKernel: x86_64 kernel boot not supported on IA32\n"));
    FreePool (StackBuffer);
    if (DeviceTreeBuffer != NULL) {
      FreePool (DeviceTreeBuffer);
    }
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_UNSUPPORTED;
#endif
  } else {
    FreePool (StackBuffer);
    if (DeviceTreeBuffer != NULL) {
      FreePool (DeviceTreeBuffer);
    }
    FreePool (BootArgs);
    FreePool (KernelBuffer);
    return EFI_UNSUPPORTED;
  }
}

STATIC
EFI_STATUS
DbtBootEntryAction (
  IN OUT  OC_PICKER_CONTEXT         *PickerContext,
  IN      EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  DEBUG ((DEBUG_INFO, "DBT: BootEntryAction picker=%p dp=%p\n",
          PickerContext, DevicePath));
  RaiseScreenVerbose ();
  DEBUG ((DEBUG_INFO, "DBT: starting DirectKernel\n"));
  //
  // Fallback: scan all filesystems for the installer kernel if no boot info
  // is bound yet.  The kernel is commonly found on a SharedSupport volume or
  // inside a MobileAsset directory, so every SimpleFileSystem handle is probed.
  //
  if ((gDbtContext != NULL) && (DbtGetInstallerDevice (gDbtContext) == NULL)) {
    CHAR16      KernelPathBuf[256];
    EFI_HANDLE  KernelDevice;

    KernelDevice = FindInstallerKernelDevice (NULL, KernelPathBuf, sizeof (KernelPathBuf));
    if (KernelDevice != NULL) {
      gInstallerDevice = KernelDevice;
      DbtSetBootInfo (gDbtContext, KernelDevice, KernelPathBuf);
      DEBUG ((DEBUG_INFO, "DBT: Fallback found kernel at %s\n", KernelPathBuf));
    } else {
      DEBUG ((DEBUG_WARN, "DBT: No installer kernel found on any volume\n"));
    }
  }

  return DirectLoadKernel (PickerContext);
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
  EFI_FILE_INFO                    *FileInfo;
  UINTN                            FileInfoSize;
  UINTN                            EntryCount;
  OC_PICKER_ENTRY                  *NewEntries;
  BOOLEAN                          IsMacSoftwareUpdate = FALSE;

  ASSERT (PickerContext != NULL);
  ASSERT (Entries != NULL);
  ASSERT (NumEntries != NULL);

  RaiseScreenVerbose ();

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
  // Look for macOS 27+ kernel/kernelcache in the installer or SharedSupport
  // volume. Classic layouts and MobileAsset layouts are probed, and the
  // kernelcache name is matched dynamically, so board specific names are not
  // hardcoded here.
  //
  if (EntryCount == 0) {
    CHAR16  KernelPathBuf[256];

    DEBUG ((DEBUG_INFO, "DBT: Probing installer volumes for kernel/kernelcache\n"));
    if (FindInstallerKernel (RootDirectory, KernelPathBuf, sizeof (KernelPathBuf))) {
      DEBUG ((DEBUG_INFO, "DBT: Found installer kernel at %s\n", KernelPathBuf));
      ++EntryCount;
      IsMacSoftwareUpdate = TRUE;
    }
  }

  DEBUG ((DEBUG_INFO, "DBT: Installer scan complete - EntryCount=%u, IsMacSoftwareUpdate=%d\n", EntryCount, IsMacSoftwareUpdate));

  if (EntryCount == 0) {
    //
    // macOS 27 Golden Gate: the installer keeps an ARM64 boot.efi, which an
    // x86_64 firmware cannot load through LoadImage.  Create the translated
    // entry; the DBT boot action discovers the kernel later.
    //
    if (IsArm64BootEfi (RootDirectory)) {
      DEBUG ((DEBUG_INFO, "DBT: ARM64 boot.efi found, creating translated installer entry\n"));
      EntryCount       = 1;
      IsMacSoftwareUpdate = TRUE;
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
    NewEntries[0].Flavour = AllocateCopyPool (AsciiStrSize ("DirectKernel"), "DirectKernel");

    //
    // All DBT entries go through unmanaged boot action to bypass LoadImage
    // since boot.efi/kernel are ARM64 and EDK2 LoadImage will reject them.
    //
    {
      CHAR16      KernelPathBuf[256];
      EFI_HANDLE  KernelDevice;

      //
      // Resolve the volume that actually holds the installer kernel (it may
      // be a separate SharedSupport volume on recent installers) and bind it
      // to the translated boot.
      //
      KernelDevice = FindInstallerKernelDevice (gInstallerDevice != NULL ? gInstallerDevice : Device,
                                                 KernelPathBuf,
                                                 sizeof (KernelPathBuf));
      if (KernelDevice != NULL) {
        gInstallerDevice = KernelDevice;
        DbtSetBootInfo (gDbtContext, KernelDevice, KernelPathBuf);
        DEBUG ((DEBUG_INFO, "DBT: Installer kernel resolved at %s\n", KernelPathBuf));
      } else {
        DbtSetBootInfo (gDbtContext, gInstallerDevice != NULL ? gInstallerDevice : Device, L"\\kernel");
      }
    }
    NewEntries[0].UnmanagedBootAction             = DbtBootEntryAction;
    NewEntries[0].UnmanagedBootGetFinalDevicePath = NULL;
    //
    // Provide file path device path + end node for the picker.
    //
    {
      EFI_DEVICE_PATH_PROTOCOL  *Dp;
      UINTN                     Size = SIZE_OF_FILEPATH_DEVICE_PATH + sizeof (CHAR16) + sizeof (EFI_DEVICE_PATH_PROTOCOL);
      Dp = AllocateZeroPool (Size);
      if (Dp != NULL) {
        FILEPATH_DEVICE_PATH  *Fp = (FILEPATH_DEVICE_PATH *)Dp;
        Fp->Header.Type    = MEDIA_DEVICE_PATH;
        Fp->Header.SubType = MEDIA_FILEPATH_DP;
        SetDevicePathNodeLength (&Fp->Header, SIZE_OF_FILEPATH_DEVICE_PATH + sizeof (CHAR16));
        Fp->PathName[0]    = L'\0';
        SetDevicePathEndNode ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Dp + SIZE_OF_FILEPATH_DEVICE_PATH + sizeof (CHAR16)));
      }
      NewEntries[0].UnmanagedDevicePath = Dp;
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

  DEBUG ((DEBUG_INFO, "DBT: entry image=%p st=%p\n", ImageHandle, SystemTable));

  gX86ImageHandle = ImageHandle;

  //
  // Raise the on-screen + file log level so the full verbose DBT trace is
  // printed to the console during the boot picker and the "macOS Installer
  // (Translated)" direct kernel run regardless of the Logging/DisplayLevel
  // setting in config.plist.
  //
  RaiseScreenVerbose ();

  Status = DbtInitContext (&gDbtContext, 0x100000);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DBT: Failed to initialize DBT context - %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "DBT: ARM64->x86_64 initialized for DirectKernel\n"));

  //
  // Install DBT fallback protocol — GUID + function pointer
  //
  {
    EFI_GUID  DbtGuid = OC_DBT_FALLBACK_PROTOCOL_GUID;
    Status = gBS->InstallMultipleProtocolInterfaces (
                     &ImageHandle,
                     &DbtGuid,
                     (VOID *)(UINTN)DbtBootEntryAction,
                     NULL
                     );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "DBT: Failed to install fallback protocol - %r\n", Status));
    } else {
      DEBUG ((DEBUG_INFO, "DBT: fallback protocol installed\n"));
    }
  }

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
  DEBUG ((DEBUG_INFO, "DBT: boot entry protocol installed\n"));
  DEBUG ((DEBUG_INFO, "DBT: driver ready\n"));

  return EFI_SUCCESS;
}
<img src="/Docs/Logos/OpenCore_with_text_Small.png" width="200" height="48"/>

[![Build Status](https://github.com/acidanthera/OpenCorePkg/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/acidanthera/OpenCorePkg/actions) [![Scan Status](https://scan.coverity.com/projects/18169/badge.svg?flat=1)](https://scan.coverity.com/projects/18169)
-----

OpenCore bootloader with development SDK.

> **Fork note (`dstatstrussia`).** This tree is an additive fork of
> [acidanthera/OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) with
> macOS 27 (Golden Gate) bring-up support. It adds the `OpenDbvX64Dxe` direct
> kernel boot driver, the `OcDbtLib` ARM64 to x86_64 dynamic binary translation
> engine, Apple Silicon SMBIOS models and hardened variable services.
> See [Fork additions](#fork-additions) below and the `OpenDbvX64Dxe` section of
> the reference manual (`Docs/Configuration.pdf`).

## Fork additions

- `Platform/OpenDbvX64Dxe` — `OC_BOOT_ENTRY_PROTOCOL` plugin that boots macOS 27
  installers directly. It finds the kernel or kernelcache on the installer media
  (classic `com.apple.installer`, `SharedSupport` MobileAsset layouts, dyld shared
  cache, plain `kernel` files and ZIP archives), registers a single
  `macOS Installer (Translated)` picker entry and either translates the ARM64
  kernel through `OcDbtLib` or starts an x86_64 kernel natively after
  `ExitBootServices`. The driver forces verbose logging while it runs; its log
  lines are prefixed with `DBT:` and `DirectKernel:`.
- `Library/OcDbtLib` — dynamic binary translation engine (ARM64 guest to x86_64
  host) with block caching, NZCV flags, system registers (PAC, SVE, FPCR/FPSR,
  TPIDR_EL1/EL0), MMIO/PCIe host layer, per-CPU slots and phys windows. Extended
  additively with Stela-inspired profiling/loop budgeting,
  `SaKongA/Stela` `feat/amdgpu-680m`-inspired Radeon 680M handoff and
  `nullmoth/nvidia-macos-driver`-inspired PCI/BAR management, GPU emulation,
  scoreboards and sleep/wake state. The translator also decodes ARMv8.1 LSE
  atomics, exclusives, CRC32/CRC32C, the long/high multiply group,
  RBIT/REV/CLZ/CLS and DMB/DSB/ISB barriers. See
  `Library/OcDbtLib/README.md` for the API and the `PcdOcDbt*` build-time
  options.
- `AppleModels` — added Apple Silicon SMBIOS definitions: `MacBookAir10,1`,
  `MacBookPro17,1`, `Macmini9,1`, `iMac21,1` and `iMac21,2`.
- `OcAppleKernelLib` — macOS 27 (Golden Gate) kernel version constants
  (`KERNEL_VERSION_GOLDENGATE` and its MIN/MAX bounds) and kernel version
  initialisation, including ARM64 `boot.efi` installers.
- Security hardening — runtime checks instead of `ASSERT`s in the variable
  subsystem, bounded Apple performance buffer parsing in `OcLogAggregatorLib`,
  and assorted compiler/codestyle fixes to keep GCC/Clang and CI builds clean.

## Libraries

This repository also contains additional UEFI support common libraries shared by other projects in [Acidanthera](https://github.com/acidanthera). The primary purpose of the library set is to provide supplemental functionality for Apple-specific UEFI drivers. Key features:

- Apple disk image loading support
- Apple keyboard input aggregation
- Apple PE image signature verification
- Apple UEFI secure boot supplemental code
- Audio management with screen reading support
- Basic ACPI and SMBIOS manipulation
- CPU information gathering with timer support
- Cryptographic primitives (SHA-256, RSA, etc.)
- Decompression primitives (zlib, lzss, lzvn, etc.)
- Helper code for ACPI reads and modifications
- Higher level abstractions for files, strings, UEFI variables
- Overflow checking arithmetics
- PE image loading with no UEFI Secure Boot conflict
- Plist configuration format parsing
- PNG image manipulation
- Text output and graphics output implementations
- XNU kernel driver injection and patch engine

Early history of the codebase could be found in [AppleSupportPkg](https://github.com/acidanthera/AppleSupportPkg) and PicoLib library set by The HermitCrabs Lab.

#### OcGuardLib

This library implements basic safety features recommended for the use within the project. It implements fast
safe integral arithmetics mapping on compiler builtins, type alignment checking, and UBSan runtime,
based on [NetBSD implementation](https://blog.netbsd.org/tnf/entry/introduction_to_µubsan_a_clean).

The use of UBSan runtime requires the use of Clang compiler and `-fsanitize=undefined` argument. Refer to
[Clang documentation](https://releases.llvm.org/7.0.0/tools/clang/docs/UndefinedBehaviorSanitizer.html) for more
details.

#### Credits

- The HermitCrabs Lab
- All projects providing third-party code (refer to file headers)
- [AppleLife](https://applelife.ru) team and user-contributed resources
- Chameleon and Clover teams for hints and legacy
- [al3xtjames](https://github.com/al3xtjames)
- [Andrey1970AppleLife](https://github.com/Andrey1970AppleLife)
- [mhaeuser (ex Download-Fritz)](https://github.com/mhaeuser)
- [Goldfish64](https://github.com/Goldfish64)
- [MikeBeaton](https://github.com/MikeBeaton)
- [nms42](https://github.com/nms42)
- [PMheart](https://github.com/PMheart)
- [savvamitrofanov](https://github.com/savvamitrofanov)
- [usr-sse2](https://github.com/usr-sse2)
- [vit9696](https://github.com/vit9696)

#### Discussion

Please refer to the following [list of OpenCore discussion forums](/Docs/FORUMS.md).

# OcDbtLib — Enhanced Dynamic Binary Translation Library

Dynamic Binary Translation (ARM64 → x86_64) library for OpenCore, extended
additively with concepts ported from three projects:

1. **Stela** (https://github.com/tylertms/Stela) — modern Rust DBT engine:
   block caching, AOT manifest concepts, PAC/SVE state, block profiles.
2. **Stela, `feat/amdgpu-680m` branch** (https://github.com/SaKongA/Stela) —
   RDNA2 "Rembrandt" 680M bring-up: PCI decode, VRAM sizing, GMC v10 DMA
   window and the Yellow Carp firmware set.
3. **nullmoth/nvidia-macos-driver** (https://github.com/nullmoth/nvidia-macos-driver)
   — Metal driver stack for NVIDIA GeForce on macOS Sequoia: PCI/BAR
   management, GSP firmware bring-up, HWRT ray-query scoreboards, sleep/wake,
   loop-budget instrumentation and OpenCore quirks.

## Layout (additive integration)

The original engine is **kept intact**:

| File | Role |
|------|------|
| `OcDbtLib.c` | Original translation engine (decoder, block cache, NZCV, sysregs, MMIO/GIC/PCIe host layer) + new profiling counters |
| `OcDbtComplex.c` | Complex-instruction dispatch helpers (existing) |
| `OcDbtEnhanced.c` | **New.** Extended surface: PCI/BAR management, GPU emulation, scoreboards, sleep/wake, quirks, PCD wiring, stats |
| `OcDbtLib.h` (local) | Internal header defining the private `DBT_CONTEXT` layout |
| `Include/Acidanthera/Library/OcDbtLib.h` | Public header (real API + extended types/constants) |
| `DbtBootExample.c`, `DbtNvidiaGpuExample.c` | Documentation-only examples, **not** in `[Sources]` |

`DBT_ARM64_STATE` was extended **at the end** of the structure (PAC keys,
`ZCR_EL1`/`SVCR`/`CNTV_TVAL_EL0`, SVE `Z/P/FFR/VL`, GPU BAR1 fields), so
existing `OFFSET_OF` users in the engine are unaffected. `DBT_CONTEXT` gained
PCI device, scoreboard, options, statistics and sleep/wake fields **before**
`CodeBuffer[0]`; `DbtInitContext` computes `TranslatedCode` from
`sizeof(DBT_CONTEXT)` at runtime, so the layout stays consistent.

## Extended API (OcDbtEnhanced.c)

### PCI devices & BAR management (NVRM-inspired)

```c
EFI_STATUS       DbtPciRegisterDevice (Ctx, VendorId, DeviceId, ClassCode,
                                       SubClass, ProgIf, RevisionId,
                                       Bus, Device, Function);
EFI_STATUS       DbtPciMapBar (Ctx, DeviceIndex, BarIndex, PhysBase, Size, HostBuffer);
EFI_STATUS       DbtPciRelocateBar1 (Ctx, DeviceIndex, NewBase, NewSize);
DBT_PCI_DEVICE  *DbtPciGetDevice (Ctx, Index);
DBT_PCI_DEVICE  *DbtPciFindDevice (Ctx, VendorId, DeviceId);
EFI_STATUS       DbtPciSetBusMaster (Ctx, DeviceIndex, Enable);
EFI_STATUS       DbtPciSetMemorySpace (Ctx, DeviceIndex, Enable);
```

### GPU emulation (rm_init_adapter / ResizeGpuBars concepts)

```c
EFI_STATUS       DbtGpuMapBar1VideoMemory (Ctx, DeviceIndex, Base, Size);
EFI_STATUS       DbtGpuLoadGspFirmware (Ctx, DeviceIndex, FirmwarePath);
UINT64           DbtGpuRegRead (Ctx, DeviceIndex, Offset);
VOID             DbtGpuRegWrite (Ctx, DeviceIndex, Offset, Value);
BOOLEAN          DbtEmulateGpuCompute (State, Inst, Ctx);
BOOLEAN          DbtEmulateRayQuery (State, Inst, Ctx);
```

`DbtGpuLoadGspFirmware` reads the image through the installer volume's
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` and stages it into the host-backed BAR1
aperture, mirroring how the NVRM kext presents the GSP firmware during
adapter init.

### AMD Radeon 680M handoff (Stela `feat/amdgpu-680m`)

```c
DBT_PCI_DEVICE  *DbtAmdFindRembrandt (Ctx);
EFI_STATUS       DbtAmdPrepareRembrandt (Ctx, DeviceIndex, VramBase, VramSize, &VramMiB);
CONST CHAR8 *CONST *DbtAmdRembrandtFirmware (&Count);
```

`DbtAmdPrepareRembrandt` accepts only PCI `1002:1681` (Radeon 680M,
"Yellow Carp"), turns on memory decoding and bus mastering the way Linux's
`pci_enable_device` does, requires the register BAR (BAR5) to be assigned,
maps the VRAM aperture into a host-backed buffer, and records the GPU
identity in `DBT_ARM64_STATE`. The constants it uses mirror Stela:

| Constant | Value | Meaning |
|----------|-------|---------|
| `DBT_AMD_REMBRANDT_680M_DEVICE_ID` | `0x1681` | The only device the RADV gate accepts |
| `DBT_AMD_REMBRANDT_BAR_REGS` | `5` | Register BAR |
| `DBT_AMD_REMBRANDT_VRAM_SIZE_REG` | `0x378C` | `mmRCC_CONFIG_MEMSIZE` (`0x0de3` dwords) |
| `DBT_AMD_REMBRANDT_DMA_LIMIT` | `2^44 - 1` | GMC v10 DMA mask |
| `DBT_AMD_REMBRANDT_FIRMWARE_COUNT` | `11` | Yellow Carp firmware images |

`DbtAmdRembrandtFirmware` returns the eleven `amdgpu/yellow_carp_*.bin`
names in the order the kernel requests them, so a boot service can stage
them into the guest firmware volume without hard-coding the list.

### Instruction-set coverage added to the translator

The engine now decodes the ARMv8.1/LSE and CRC extensions that the kernel
and kexts execute during early boot:

- **Atomics** — `LDADD`/`LDCLR`/`LDEOR`/`LDSET`/`LDSMAX`/`LDSMIN`/
  `LDUMAX`/`LDUMIN`/`SWP` (all sizes, all orderings), `CAS`/`CASA`/
  `CASL`/`CASAL`, and `LDAPR`.
- **Exclusives** — `LDXR`/`LDAXR` and `STXR`/`STLXR`, with a real monitor
  (`DbtExclusiveLoad`/`DbtExclusiveStore`, compare-exchange backed).
- **Ordered accesses** — `LDAR`/`STLR` (x86 TSO provides the ordering).
- **Multiplies** — `MADD`/`MSUB`/`MUL`/`MNEG` plus the formerly unreachable
  `SMADDL`/`SMSUBL`/`SMULL`/`SMNEGL`/`SMULH`/`UMADDL`/`UMSUBL`/`UMULL`/
  `UMNEGL`/`UMULH` group.
- **Bit manipulation** — `RBIT`, `REV`, `REV16`, `REV32`, `CLZ`, `CLS`.
- **Checksums** — `CRC32B/H/W/X` and `CRC32CB/CH/CW/CX`.
- **Barriers** — `DMB`/`DSB` → `MFENCE`, `ISB` → `LFENCE`; `WFI`/`WFE`/
  `SEV`/`BTI` remain NOPs for the single-vCPU guest.

Multiply, CRC, RBIT, atomics and the exclusive monitor are small C helpers
called from the JIT (MS x64 ABI: RCX, RDX, R8, R9); `CLZ`/`CLS`/`REV`/
`REV16`/`REV32` are emitted inline (`BSR`, `BSWAP`, masks). The encodings
and masks were verified against `clang --target=aarch64-unknown-none`
output, and the helpers are covered by host-side vector tests (CRC32 and
CRC32C check values, `SMULH`/`UMULH` against a 128-bit reference, RMW and
CAS semantics).

### Scoreboards (HWRT-inspired)

```c
DBT_SCOREBOARD  *DbtScoreboardCreate (Ctx, Name);
VOID             DbtScoreboardSignal (Ctx, Index);
VOID             DbtScoreboardWait (Ctx, Index, Timeout);
BOOLEAN          DbtScoreboardSignaled (Ctx, Index);
```

### Sleep/wake (NVRM-inspired)

```c
EFI_STATUS       DbtSaveState (Ctx, &Buffer, &BufferSize);
EFI_STATUS       DbtRestoreState (Ctx, Buffer, BufferSize);
EFI_STATUS       DbtSuspend (Ctx);
EFI_STATUS       DbtResume (Ctx);
```

The serialised blob carries magic/version, the full `DBT_ARM64_STATE`,
`SysRegs[256]`, translation options, profiling counters, PCI device metadata
and scoreboard state. Host allocations (`Bar1HostBuffer`) are **not** carried
across sleep and must be re-established after wake, exactly like NVRM
re-initialises the BAR window.

### OpenCore quirks integration

```c
VOID             DbtApplyQuirks (Ctx, Quirks);
UINT32           DbtGetQuirks (Ctx);
```

| Quirk flag | Value | Mirrors |
|------------|-------|---------|
| `DBT_QUIRK_RESIZE_GPU_BARS` | 0x1 | `ResizeGpuBars` |
| `DBT_QUIRK_RESIZE_APPLE_GPU_BARS` | 0x2 | `ResizeAppleGpuBars` |
| `DBT_QUIRK_AMFI_DISABLE` | 0x4 | `amfi_get_out_of_my_way=0x1` |
| `DBT_QUIRK_SIP_DISABLE` | 0x8 | `csr-active-config` |
| `DBT_QUIRK_NV_OFF` | 0x10 | `-nvoff` boot-arg |
| `DBT_QUIRK_NV_KMS_NO_SMOOTH` | 0x20 | `-nvkmsnosmooth` boot-arg |

### Options, profiling, diagnostics

```c
VOID             DbtSetLoopBudget (Ctx, Budget);
VOID             DbtSetVerbose (Ctx, Enable);
VOID             DbtSetTraceMem (Ctx, Enable);
VOID             DbtApplyPcds (Ctx);
VOID             DbtGetStats (Ctx, &TotalTranslatedBlocks, &TotalExecutedBlocks,
                              &CacheHits, &CacheMisses, &LoopBudgetExhausted);
VOID             DbtDumpPciDevices (Ctx);
VOID             DbtDumpScoreboards (Ctx);
```

The engine in `OcDbtLib.c` now maintains the counters itself:
`TotalExecutedBlocks` increments per `DbtExecute`, `CacheHits`/`CacheMisses`
per `DbtTranslateBlock`, and `DefaultLoopBudget` feeds the chain-loop
watchdog (`gDbtChainBudget`), mirroring the nullmoth translator's
`DEFAULT_LOOP_BUDGET` (0x10000).

### Hardware emulation (already in the engine, now public API)

```c
EFI_STATUS       DbtMmioRegister (Ctx, Name, PhysBase, Size, Read, Write);
EFI_STATUS       DbtAddDevice (Ctx, Name);
EFI_STATUS       DbtGicSetPriority (Ctx, InterruptId, Priority);
EFI_STATUS       DbtPcieBarMap (Ctx, BarIndex, PhysBase, Size);
```

## Configuration PCDs (OpenCorePkg.dec, tokens 0x0000060b–0x00000612)

| PCD | Type | Default | Purpose |
|-----|------|---------|---------|
| `PcdOcDbtVerbose` | BOOLEAN | FALSE | Verbose translation tracing |
| `PcdOcDbtTraceMem` | BOOLEAN | FALSE | Memory-access tracing |
| `PcdOcDbtEnableGpu` | BOOLEAN | FALSE | GPU emulation surface |
| `PcdOcDbtEnableRayQuery` | BOOLEAN | FALSE | RT-core ray-query path |
| `PcdOcDbtEnablePac` | BOOLEAN | TRUE | PAC emulation |
| `PcdOcDbtEnableSve` | BOOLEAN | FALSE | SVE support |
| `PcdOcDbtLoopBudget` | UINT32 | 0x10000 | Chain watchdog budget |
| `PcdOcDbtQuirks` | UINT32 | 0x0 | `DbtApplyQuirks` mask |

`DbtApplyPcds(Ctx)` applies these to a live context (flags, loop budget,
quirks); `PcdOcDbtEnableGpu/RayQuery/Pac/Sve` document the intended gating
for future automatic dispatch wiring in `OcDbtComplex.c`.

## Example: NVIDIA bring-up (abridged)

```c
DbtInitContext (&Ctx, 0x200000);
DbtApplyQuirks (Ctx, DBT_QUIRK_RESIZE_GPU_BARS | DBT_QUIRK_RESIZE_APPLE_GPU_BARS);

DbtPciRegisterDevice (Ctx, 0x10DE, 0x2A82, 0x03, 0x02, 0x00, 0xA1, 0, 1, 0);
DbtPciRelocateBar1 (Ctx, 0, 0xF000000000ULL, 0x10000000ULL);
DbtGpuMapBar1VideoMemory (Ctx, 0, 0xF000000000ULL, 0x10000000ULL);
DbtPciSetBusMaster (Ctx, 0, TRUE);
DbtPciSetMemorySpace (Ctx, 0, TRUE);
DbtGpuLoadGspFirmware (Ctx, 0, L"\\EFI\\OC\\NVIDIA\\gsp_fw.bin");
DbtScoreboardCreate (Ctx, L"GPU_COMMAND_COMPLETE");
DbtSetLoopBudget (Ctx, DBT_DEFAULT_LOOP_BUDGET);

/* boot the kernel through the standard DbtSetSegments/DbtExecute flow */
```

## Example: AMD Radeon 680M bring-up (abridged)

```c
DbtInitContext (&Ctx, 0x200000);

DbtPciRegisterDevice (Ctx, 0x1002, 0x1681, 0x03, 0x00, 0x00, 0xC1, 0, 2, 0);
/* BAR5 (registers) is assigned by the caller's PCI scan. */
DbtPciMapBar (Ctx, 0, DBT_AMD_REMBRANDT_BAR_REGS, 0xF0100000ULL, 0x80000ULL, NULL);

UINT32 VramMiB;
Status = DbtAmdPrepareRembrandt (Ctx, 0, 0xF800000000ULL, 0x40000000ULL, &VramMiB);
if (EFI_ERROR (Status)) {
  /* not a 680M, or the register BAR is unassigned */
}

UINTN Count;
CONST CHAR8 *CONST *Names = DbtAmdRembrandtFirmware (&Count);  /* 11 images */
```

Full reference flows: `DbtBootExample.c` and `DbtNvidiaGpuExample.c`
(documentation-only; excluded from `[Sources]`).

## References

- Stela — https://github.com/tylertms/Stela
- Stela, `feat/amdgpu-680m` — https://github.com/SaKongA/Stela/tree/feat/amdgpu-680m
- nullmoth/nvidia-macos-driver — https://github.com/nullmoth/nvidia-macos-driver
- NVIDIA open-gpu-kernel-modules — https://github.com/NVIDIA/open-gpu-kernel-modules
- ARM ARMv8-A Architecture Reference Manual (PAC, SVE)
- License: BSD-2-Clause-Patent (same as OpenCore)

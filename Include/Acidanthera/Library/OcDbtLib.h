/** @file
   Copyright (C) 2026. All rights reserved.

   Dynamic Binary Translation Library for ARM64 to x86_64
**/

#ifndef OC_DBT_LIB_H
#define OC_DBT_LIB_H

#include <Uefi.h>

#define DBT_MAX_PHYS_WINDOWS  4
#define DBT_MAX_DEVICES      16

//
// Enhanced configuration (Stela + nullmoth/nvidia-macos-driver inspired):
// PCI device/BAR management, HWRT-style scoreboards and loop budgeting.
//
#define DBT_MAX_PCI_BARS        6
#define DBT_MAX_PCI_DEVICES     8
#define DBT_MAX_SCOREBOARDS     8
#define DBT_DEFAULT_LOOP_BUDGET 0x10000

//
// Translation flags (see DbtSetVerbose / DbtSetTraceMem)
//
#define DBT_TRANSLATE_FLAG_VERBOSE   0x00000001
#define DBT_TRANSLATE_FLAG_TRACE_MEM 0x00000002

//
// OpenCore quirks consumed by DbtApplyQuirks, mirroring the requirements of
// the nullmoth/nvidia-macos-driver (ResizeGpuBars, ResizeAppleGpuBars,
// csr-active-config, amfi, -nvoff, -nvkmsnosmooth).
//
#define DBT_QUIRK_RESIZE_GPU_BARS       0x00000001
#define DBT_QUIRK_RESIZE_APPLE_GPU_BARS 0x00000002
#define DBT_QUIRK_AMFI_DISABLE          0x00000004
#define DBT_QUIRK_SIP_DISABLE           0x00000008
#define DBT_QUIRK_NV_OFF                0x00000010
#define DBT_QUIRK_NV_KMS_NO_SMOOTH      0x00000020

/**
  ARM64 CPU state for translation. Registers stored in memory for save/restore.
  XZR (register 31) always reads as 0 and ignores writes.
**/
typedef struct {
  UINT64 X[31];     // X0-X30
  UINT64 SP;        // Stack pointer
  UINT64 PC;        // Program counter
  UINT64 PSTATE;    // NZCV flags at bits 31-28, mode/DAIF at lower bits
  UINT64 SP_EL0;    // EL0 stack pointer
  UINT64 SP_EL1;    // EL1 stack pointer
  UINT64 ELR_EL1;   // Exception link register
  UINT64 SPSR_EL1;  // Saved PSTATE
  UINT64 PAR_EL1;   // AT-translation result (PA==VA emulation)
  //
  // System register stubs
  //
  UINT64 SCTLR_EL1;
  UINT64 TTBR0_EL1;
  UINT64 TTBR1_EL1;
  UINT64 TCR_EL1;
  UINT64 MAIR_EL1;
  UINT64 VBAR_EL1;
  UINT64 ESR_EL1;
  UINT64 FAR_EL1;
  UINT64 MIDR_EL1;
  UINT64 MPIDR_EL1;
  UINT64 CNTFRQ_EL0;
  UINT64 FPCR;      // Floating Point Control Register
  UINT64 FPSR;      // Floating Point Status Register
  UINT64 CNTVCT_EL0;
  UINT64 CNTV_CTL_EL0;
  UINT64 CNTV_CVAL_EL0;
  UINT64 ACTLR_EL1;
  UINT64 CPACR_EL1;
  UINT64 TPIDR_EL1;    // Per-CPU data base (thread pointer, EL1)
  UINT64 TPIDR_EL0;
  UINT64 TPIDRRO_EL0;
  //
  // NEON/SIMD register file (Q0-Q31, 128 bits each)
  //
  UINT64 Qlo[32];   // Vn.2D[0]
  UINT64 Qhi[32];   // Vn.2D[1]
  //
  // Enhanced (Stela-inspired): pointer authentication keys (ARMv8.3 PAC).
  // Appended at the end of the structure so existing OFFSET_OF users are
  // not affected.
  //
  UINT64 APIAKEYLO_EL1;
  UINT64 APIAKEYHI_EL1;
  UINT64 APIBKEYLO_EL1;
  UINT64 APIBKEYHI_EL1;
  UINT64 APDAKEYLO_EL1;
  UINT64 APDAKEYHI_EL1;
  UINT64 APDBKEYLO_EL1;
  UINT64 APDBKEYHI_EL1;
  UINT64 APGAKEYLO_EL1;
  UINT64 APGAKEYHI_EL1;
  //
  // Enhanced: SVE/SME control and generic-timer compare value
  //
  UINT64 ZCR_EL1;      // SVE vector length control
  UINT64 SVCR;         // SME streaming-mode control
  UINT64 CNTV_TVAL_EL0; // Timer countdown value
  //
  // Enhanced: SVE vector registers (Z0-Z31, up to 256-bit), predicate
  // registers and first-fault register.
  //
  UINT64 Z[32][4];
  UINT64 P[16];
  UINT64 FFR[4];
  UINT64 VL;           // current SVE vector length in bits (0 = disabled)
  //
  // Enhanced (NVRM-inspired): GPU PCI identity and BAR1 video-memory window
  //
  UINT64 GpuBar1Base;  // GPU BAR1 (video memory) guest base address
  UINT64 GpuBar1Size;  // GPU BAR1 size
  UINT32 GpuDeviceId;  // NVIDIA device ID (e.g. 0x2A82)
  UINT32 GpuVendorId;  // NVIDIA vendor ID (0x10DE)
  UINT32 GpuClassCode; // PCI class code
  UINT32 GpuRevisionId;// GPU revision ID
} DBT_ARM64_STATE;

/**
  Compatibility alias
**/
typedef DBT_ARM64_STATE DBT_ARM64_CONTEXT;

typedef struct {
  UINT32 GICD_CTLR;   // Distributor control
  UINT32 GICD_TYPER;  // Distributor type
  UINT32 GICC_CTLR;   // CPU interface control
  UINT32 GICC_PMR;    // Priority mask
  UINT32 GICC_BPR;    // Binary point
  UINT32 GICC_RPR;    // Running priority
  UINT32 GICC_HPPIR;  // Highest pending interrupt
  UINT32 InterruptSet; // Pending interrupts bitmap
} DBT_GIC_STATE;

typedef struct DBT_MMIO_TRAP {
  UINT64 BaseAddr;
  UINT64 Size;
  VOID  (*ReadHandler)(UINT64 Offset, UINT32 Size, UINT64 *Value);
  VOID  (*WriteHandler)(UINT64 Offset, UINT32 Size, UINT64 Value);
  VOID  *Context;
  BOOLEAN Active;
} DBT_MMIO_TRAP;

typedef struct DBT_PCIE_BAR {
  UINT64 PhysBase;
  UINT64 Size;
  UINT32 BarIndex;
  BOOLEAN Enabled;
  UINT8  *HostMapping;
} DBT_PCIE_BAR;

typedef struct DBT_DEVICE {
  CONST CHAR16 *Name;    // Device name (NVMe / SATA / USB / GPU etc.)
  DBT_MMIO_TRAP *Traps;  // MMIO trap array
  UINTN TrapCount;
  DBT_PCIE_BAR *Bars;    // PCIe BAR mapping
  UINTN BarCount;
  DBT_GIC_STATE *Gic;    // Associated GIC state
  BOOLEAN Registered;
} DBT_DEVICE;

//
// Enhanced (NVRM-inspired, nullmoth/nvidia-macos-driver): PCI device with
// full BAR management, including BAR1 video-memory relocation
// (ResizeGpuBars concept).
//
typedef struct DBT_PCI_DEVICE {
  UINT16 VendorId;
  UINT16 DeviceId;
  UINT16 Command;
  UINT16 Status;
  UINT8  RevisionId;
  UINT8  ClassCode;
  UINT8  SubClass;
  UINT8  ProgIf;
  UINT8  HeaderType;
  UINT8  Bus;
  UINT8  Device;
  UINT8  Function;
  UINT64 Bars[DBT_MAX_PCI_BARS];
  UINTN  BarSizes[DBT_MAX_PCI_BARS];
  UINT32 InterruptLine;
  UINT32 InterruptPin;
  BOOLEAN BusMasterEnabled;
  BOOLEAN MemorySpaceEnabled;
  BOOLEAN IoSpaceEnabled;
  BOOLEAN Bar1Relocated;    // BAR1 moved away from the firmware console range
  UINT8  *Bar1HostBuffer;   // host-backed BAR1 video memory
} DBT_PCI_DEVICE;

//
// Enhanced (HWRT-inspired, nullmoth/nvidia-macos-driver): scoreboard for
// asynchronous GPU / RT-core synchronisation.
//
typedef struct DBT_SCOREBOARD {
  UINT64 Value;
  UINT64 WaitMask;
  UINT64 SignalMask;
  BOOLEAN Active;
  CONST CHAR16 *Name;
} DBT_SCOREBOARD;

/**
  DBT translation context (opaque)
**/
typedef struct DBT_CONTEXT DBT_CONTEXT;

EFI_STATUS
DbtInitContext (
  OUT DBT_CONTEXT  **Context,
  IN  UINTN         CodeSize
  );

EFI_STATUS
DbtSetBootInfo (
  IN DBT_CONTEXT  *Context,
  IN EFI_HANDLE   InstallerDevice,
  IN CONST CHAR16 *KernelPath
  );

EFI_STATUS
DbtSetSegments (
  IN DBT_CONTEXT  *Context,
  IN UINTN         SegCount,
  IN UINT64       *SegVmAddr,
  IN UINT64       *SegVmSize,
  IN UINT64       *SegFileOff,
  IN VOID         *KernelBuffer
  );

EFI_STATUS
DbtSetPhysWindow (
  IN DBT_CONTEXT  *Context,
  IN UINT64        Base,
  IN UINTN         Size,
  IN VOID         *Buffer
  );

EFI_HANDLE
DbtGetInstallerDevice (
  IN DBT_CONTEXT  *Context
  );

CONST CHAR16 *
DbtGetKernelPath (
  IN DBT_CONTEXT  *Context
  );

VOID
DbtExecute (
  IN DBT_CONTEXT       *Context,
  IN DBT_ARM64_CONTEXT *ArmContext
  );

UINT64
DbtTranslateVaToPa (
  IN DBT_CONTEXT  *Context,
  IN UINT64       Va
  );

UINT64
DbtHandleException (
  IN DBT_CONTEXT  *Context,
  IN UINT64       ExceptionType,
  IN UINT64       FaultAddr,
  IN UINT64       CurrentPc
  );

EFI_STATUS
DbtTranslateBlock (
  IN OUT DBT_CONTEXT  *Context,
  IN     VOID         *ArmCode,
  IN     UINTN         CodeSize,
  IN     UINT64        BaseAddr,
  OUT    VOID         *X86Code  OPTIONAL
  );

BOOLEAN
DbtBlockCached (
  IN DBT_CONTEXT  *Context,
  IN UINT64        Pc
  );

VOID
DbtFreeContext (
  IN DBT_CONTEXT  *Context
  );

//
// ===========================================================================
// Enhanced API (Stela + nullmoth/nvidia-macos-driver inspired).
// Implemented in OcDbtEnhanced.c unless noted otherwise.
// ===========================================================================
//

//
// Hardware emulation entry points already implemented in OcDbtLib.c
// (declared here so all consumers can reach them through the public header).
//
EFI_STATUS
DbtMmioRegister (
  IN DBT_CONTEXT  *Context,
  IN CONST CHAR16 *Name,
  IN UINT64        PhysBase,
  IN UINT64        Size,
  IN VOID        (*Read) (UINT64 Offset, UINT32 Size, UINT64 *Value),
  IN VOID        (*Write) (UINT64 Offset, UINT32 Size, UINT64 Value)
  );

EFI_STATUS
DbtAddDevice (
  IN DBT_CONTEXT  *Context,
  IN CONST CHAR16 *Name
  );

EFI_STATUS
DbtGicSetPriority (
  IN DBT_CONTEXT  *Context,
  IN UINT32        InterruptId,
  IN UINT8         Priority
  );

EFI_STATUS
DbtPcieBarMap (
  IN DBT_CONTEXT  *Context,
  IN UINT32        BarIndex,
  IN UINT64        PhysBase,
  IN UINT64        Size
  );

//
// PCI device & BAR management (NVRM-inspired)
//
EFI_STATUS
DbtPciRegisterDevice (
  IN DBT_CONTEXT  *Context,
  IN UINT16        VendorId,
  IN UINT16        DeviceId,
  IN UINT8         ClassCode,
  IN UINT8         SubClass,
  IN UINT8         ProgIf,
  IN UINT8         RevisionId,
  IN UINT8         Bus,
  IN UINT8         Device,
  IN UINT8         Function
  );

EFI_STATUS
DbtPciMapBar (
  IN DBT_CONTEXT  *Context,
  IN UINTN         DeviceIndex,
  IN UINT32        BarIndex,
  IN UINT64        PhysBase,
  IN UINTN         Size,
  IN VOID         *HostBuffer  OPTIONAL
  );

EFI_STATUS
DbtPciRelocateBar1 (
  IN DBT_CONTEXT  *Context,
  IN UINTN         DeviceIndex,
  IN UINT64        NewBase,
  IN UINTN         NewSize
  );

DBT_PCI_DEVICE *
DbtPciGetDevice (
  IN DBT_CONTEXT  *Context,
  IN UINTN         Index
  );

DBT_PCI_DEVICE *
DbtPciFindDevice (
  IN DBT_CONTEXT  *Context,
  IN UINT16        VendorId,
  IN UINT16        DeviceId
  );

EFI_STATUS
DbtPciSetBusMaster (
  IN DBT_CONTEXT  *Context,
  IN UINTN         DeviceIndex,
  IN BOOLEAN       Enable
  );

EFI_STATUS
DbtPciSetMemorySpace (
  IN DBT_CONTEXT  *Context,
  IN UINTN         DeviceIndex,
  IN BOOLEAN       Enable
  );

//
// GPU emulation (NVRM rm_init_adapter / ResizeGpuBars concepts)
//
EFI_STATUS
DbtGpuMapBar1VideoMemory (
  IN DBT_CONTEXT  *Context,
  IN UINTN         DeviceIndex,
  IN UINT64        Base,
  IN UINTN         Size
  );

EFI_STATUS
DbtGpuLoadGspFirmware (
  IN DBT_CONTEXT   *Context,
  IN UINTN          DeviceIndex,
  IN CONST CHAR16  *FirmwarePath
  );

UINT64
DbtGpuRegRead (
  IN DBT_CONTEXT  *Context,
  IN UINTN         DeviceIndex,
  IN UINT64        Offset
  );

VOID
DbtGpuRegWrite (
  IN DBT_CONTEXT  *Context,
  IN UINTN         DeviceIndex,
  IN UINT64        Offset,
  IN UINT64        Value
  );

//
// GPU compute / RT-core ray query instruction emulation
//
BOOLEAN
DbtEmulateGpuCompute (
  IN OUT DBT_ARM64_STATE *State,
  IN     UINT32           Inst,
  IN     DBT_CONTEXT     *Context
  );

BOOLEAN
DbtEmulateRayQuery (
  IN OUT DBT_ARM64_STATE *State,
  IN     UINT32           Inst,
  IN     DBT_CONTEXT     *Context
  );

//
// Scoreboard synchronisation (HWRT-inspired)
//
DBT_SCOREBOARD *
DbtScoreboardCreate (
  IN DBT_CONTEXT   *Context,
  IN CONST CHAR16  *Name
  );

VOID
DbtScoreboardSignal (
  IN DBT_CONTEXT  *Context,
  IN UINT32        ScoreboardIndex
  );

VOID
DbtScoreboardWait (
  IN DBT_CONTEXT  *Context,
  IN UINT32        ScoreboardIndex,
  IN UINT64        Timeout
  );

BOOLEAN
DbtScoreboardSignaled (
  IN DBT_CONTEXT  *Context,
  IN UINT32        ScoreboardIndex
  );

//
// Sleep/wake support (NVRM-inspired)
//
EFI_STATUS
DbtSaveState (
  IN DBT_CONTEXT  *Context,
  OUT VOID       **Buffer,
  OUT UINTN       *BufferSize
  );

EFI_STATUS
DbtRestoreState (
  IN DBT_CONTEXT  *Context,
  IN VOID         *Buffer,
  IN UINTN         BufferSize
  );

EFI_STATUS
DbtSuspend (
  IN DBT_CONTEXT  *Context
  );

EFI_STATUS
DbtResume (
  IN DBT_CONTEXT  *Context
  );

//
// OpenCore quirks integration (ResizeGpuBars, SIP, AMFI, -nvoff, ...)
//
VOID
DbtApplyQuirks (
  IN DBT_CONTEXT  *Context,
  IN UINT32        Quirks
  );

UINT32
DbtGetQuirks (
  IN DBT_CONTEXT  *Context
  );

//
// Translation options, profiling and diagnostics
//
VOID
DbtSetLoopBudget (
  IN DBT_CONTEXT  *Context,
  IN UINT32        Budget
  );

VOID
DbtSetVerbose (
  IN DBT_CONTEXT  *Context,
  IN BOOLEAN       Enable
  );

VOID
DbtSetTraceMem (
  IN DBT_CONTEXT  *Context,
  IN BOOLEAN       Enable
  );

VOID
DbtApplyPcds (
  IN DBT_CONTEXT  *Context
  );

VOID
DbtGetStats (
  IN DBT_CONTEXT  *Context,
  OUT UINT64      *TotalTranslatedBlocks  OPTIONAL,
  OUT UINT64      *TotalExecutedBlocks    OPTIONAL,
  OUT UINT64      *CacheHits              OPTIONAL,
  OUT UINT64      *CacheMisses            OPTIONAL,
  OUT UINT64      *LoopBudgetExhausted    OPTIONAL
  );

VOID
DbtDumpPciDevices (
  IN DBT_CONTEXT  *Context
  );

VOID
DbtDumpScoreboards (
  IN DBT_CONTEXT  *Context
  );

#endif // OC_DBT_LIB_H
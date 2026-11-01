/** @file
   Copyright (C) 2026. All rights reserved.

   Dynamic Binary Translation Library for ARM64 to x86_64
**/

#ifndef OC_DBT_LIB_H
#define OC_DBT_LIB_H

#include <Uefi.h>

#define DBT_MAX_PHYS_WINDOWS  4
#define DBT_MAX_DEVICES      16

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

#endif // OC_DBT_LIB_H
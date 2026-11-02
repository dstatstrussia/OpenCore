/**
  OcDbtComplex.c — complex AArch64 instruction helpers for DBT.
  Host-side helper invoked by emitted x86 code via ABI-agnostic globals.
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include "OcDbtLib.h"
#include "OcDbtComplex.h"

DBT_ARM64_STATE *gDbtComplexInst = NULL;
UINT32          gDbtComplexInstCode = 0;

VOID DbtExecComplex (VOID) {
  UINT32 InstCode = gDbtComplexInstCode;
  DBT_ARM64_STATE *S = gDbtComplexInst ? gDbtComplexInst : gDbtActiveState;

  STATIC BOOLEAN Once = FALSE;
  if (!Once) {
    Once = TRUE;
    DEBUG ((DEBUG_INFO, "DBT_COMPLEX: first complex instruction PC=0x%llx code=0x%x (host helper active)\n",
            (UINT64)(UINTN)(S ? S->PC : 0), InstCode));
  }

  DEBUG ((DEBUG_INFO, "DBT_COMPLEX: PC=0x%llx code=0x%x helper called\n",
          (UINT64)(UINTN)(S ? S->PC : 0), InstCode));
}

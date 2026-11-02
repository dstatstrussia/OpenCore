/**
  OcDbtComplex — host-side complex-instruction helpers for DBT.
  Called by emitted x86 code through ABI-agnostic module globals.
**/
#ifndef OC_DBT_COMPLEX_H
#define OC_DBT_COMPLEX_H

#include <Uefi.h>
#include <Library/OcDbtLib.h>

VOID DbtExecComplex (VOID);

// Expose active state / context to emitted x86 (ABI-agnostic)
extern DBT_ARM64_STATE *gDbtComplexInst;
extern UINT32          gDbtComplexInstCode;
extern DBT_ARM64_STATE *gDbtActiveState;

#endif

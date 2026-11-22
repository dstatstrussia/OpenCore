# Anathema study — OpenCore fork improvements

Source: https://github.com/riftaway7-code/anathema (Rust workspace, cloned at
`Desktop/anathema/src`).  Anathea is a *VM*: AArch64 emulator with a software
memory model and its own Metal-free renderer, booted from a UEFI loader
(`anathema-boot`).  The OpenCore fork has `OcDbtLib`, a C DBT (AArch64→x86-64
dynamic binary translator) used by `OpenDbvX64Dxe`.  Both projects translate
AArch64, so this compares the translators and picks up what is missing in the
fork.

## Coverage comparison (anathema-xlat/decode.rs vs Library/OcDbtLib/OcDbtLib.c)

| group | anathema-xlat | OcDbtLib | status |
| --- | --- | --- | --- |
| integer ALU (dp reg/imm, cmp, csel, svc) | full | full | even |
| FP + SIMD | full decoder, FP/NEON translated (`VLog`, `VArith`, `VmovI`, `Vdup`, `Vtbl`, AES/SHA crypto, `Movi`, `Fmov*`, …) | FP/SIMD = NOP stubs (`DBT_SIMD: FADD/FMUL/FSUB → NOP`, `SIMD op → NOP`) | **fork behind** |
| atomics | `LDADD/LDCLR/LDEOR/LDSET`, `SMIN/SMAX/UMIN/UMAX/SWP`, `LDAPR`, CAS/CASA/CASL/CASAL | same + LDAPR (lines ~2087) | even/ahead |
| exclusives | LDXR/STXR, LDAXR/STAXR | yes (`Ldex/Stex`) | even |
| system instrs | Mrs/Msr/Sys/Hint/MsrImm/Barrier/Pac | same | even |
| branches | B/BL/BR/BLR/RET/ERET/CBZ/TBZ/B.cond | yes | even |
| load/store | Ld/St/Ldp/Stp (+SIMD vldst) | yes | even |
| barriers | DMB/DSB/ISB/Clrex | yes | even |
| SVE/SME/SMT | Unsup | Unsup | equal |
| guest MMU | 16K granule walker, permission/access-fault, AT/PAR, DC ZVA (rt) | MMIO-trap based shadow MMU, guest-physical windows | different design |

## Findings

1. **FP/SIMD translation is the main gap.**  Anathea translates a large part of
   NEON; the fork NOPs most SIMD/FP (`OcDbtLib.c:3727-3776`).  This is the
   first improvement candidate (see roadmap below).
2. **Anathea decodes `DC`/`IC` system instructions only as `Unsup`**
   (anathema `decode.rs::system()`: `match crn { 2…4, _ => Unsup("system") }`).
   The fork is the same there — no regression, no port needed.
3. **Atomics parity is good.**  Fork covers LDAPR, all LSE forms and
   CAS/CASA/CASL/CASAL with acquire/release ordering; anathema's
   `AtomOp` set matches.
4. **Anathea is a VM, not a DBT**: software MMU with TLB, permission faults,
   DC ZVA, faulting `AT`/`PAR`, EL0/EL1 with `SVC`/`ERET` banking — while the
   fork is a DBT with MMIO traps + shadow MMU; the designs differ, so this is
   where a port is hard.  Anathea's VM does implement full FP/NEON semantics
   via `anathema-rt/src/simd.rs` (1227 lines) — that is the FP/SIMD work needed
   for the fork.

## Improvement roadmap (from anathema)

1. **FP/SIMD translation (anathema-xlat/src/trans/vec.rs + anathema-rt/src/simd.rs)**
   — port `VLog` (AND/ORR/EOR/BIC/ORN/BSL/BIT/BIF/CMV), `VArith` (add/sub/mul/
   shift lanes), `VmovI`, `Vdup`, `Vtbl`, AES/SHA crypto ops into
   `OcDbtLib` + its x86 emitter.  Biggest single win for macOS 27 kernel boot
   (kernel uses NEON early).
2. **`DC ZVA` guest semantics** — anatomical implements DC ZVA in its MMU
   (`anathema-rt/src/mmu.rs`); the fork should zero the guest memory for the
   DBT when translated.
3. **Adopt anathema's debug harness ideas**: `anathema-xlat/src/oracle.rs`
   (qemu-user oracle fuzzing) for the fork's translator — there is no fork
   equivalent; many other useful dev tools (`rt-boot`, `fbcheck`, `acdtinfo`,
   `kpdescribe`).

## Compile note

`Platform/OpenDbvX64Dxe/OpenDbvX64Dxe.c` builds with the EDK2 clang toolchain:
```
cd UDK/Build/OpenCorePkg/DEBUG_CLANGPDB/X64/OpenCorePkg/Platform/OpenDbvX64Dxe/OpenDbvX64Dxe
make tbuild
```

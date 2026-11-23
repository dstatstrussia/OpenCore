# Universal arm64 -> x86_64 translations ("hex and obj" hybrid solution)

Universal, hybrid solution that makes macOS ARM (Golden Gate, and any other
Apple Silicon arm64 macOS) run on x86_64: the ARM64 code executes as real
x86_64 machine code - the universal translations - plus the usual OpenCore
hex patch data format for anything that needs to be applied on top.

## What is here

| File | Form | Contents |
|---|---|---|
| `translations.nasm` | source | The x86_64 snippets that run where each arm64 form does (the "obj" source) |
| `translations.o` | obj | Assembled x86_64 snippets (the "obj", built by `generate.sh`) |
| `translations.h` | hex | The byte arrays of the same snippets (the "hex"), plus the guest->host register map and the translation table |
| `generate.sh` | tool | Regenerates `translations.o` from `translations.nasm` |

The hex form and the obj form are consistent: `nasm -f bin
translations.nasm -o /dev/stdout | xxd -i` gives the combined bytes of the
snippets that `translations.o` contains in its `.text`.

## Translations

Per-instruction-form universal translations (universal across all Apple
Silicon arm64 macOS builds, since they are per form, not per version):

| arm64 form | x86_64 translation (verified with objdump) |
|---|---|
| `nop` | `nop` |
| `ret` | `ret` |
| `b <imm26>` | `jmp rel32` (E9 + rel32, patch rel32 to the translated destination) |
| `bl <imm26>` | `call rel32` (E8 + rel32) |
| `movz x0, #imm16, LSL #0` | `movabs rax, imm64` (48 B8 + imm64; the consumer sets imm64) |
| `mov x0, x1` | `mov rax, rbx` |
| `ldr x0, [sp, #0x10]` | `mov rax, qword [rsp + 0x10]` |
| `str x0, [sp, #0x10]` | `mov qword [rsp + 0x10], rax` |

The guest->host register map (`mArm64ToX8664RegMap`) maps arm64 `x0..x13`
onto the host registers the snippets are written against
(`rax, rbx, rcx, rdx, rsi, rdi, r8..r15`), `sp` onto `rsp`; the consumer
patches any offsets/imm64s it needs.

## Usage

```sh
./generate.sh          # rebuild translations.o
```

The tables in `translations.h` are ready to use: iterate
`mArm64ToX8664` and copy each `Code`/`CodeSize` to the guest's execution
stream in place of the guest arm64 bytes, patching branch displacements,
as Stela's compiler does; new forms are added as new
`mTransArm64Xxx` snippets in `translations.nasm` and
`translations.h`.

## Relation to Stela

Stela-Navi22's `crates/stela-engine/src/translations.rs` is the full
translator: a JIT/AOT arm64e -> x86_64 compiler covering ~3,000 AArch64
instruction encodings, with register mapping and cached translations.
This package provides the universal hex+obj translation data (per-form,
version-neutral) for the same goal.

## Notes

- The snippets are hand-verified machine code (objdump disassembly above);
  verify with `objdump -d translations.o` after changes.
- Branch snippets use a zero rel32 with the destination patching
  convention of Stela.

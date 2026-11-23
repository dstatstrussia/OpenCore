; ---------------------------------------------------------------------------
; Universal arm64 -> x86_64 translation snippets (Stela-style hybrid data)
;
; These are the "obj" part of the hybrid solution: real x86_64 machine code,
; assembled from this file with `nasm -f elf64` to produce translations.o.
; The hex form of the same bytes is generated into translations.h by
; generate.sh, and the OC_ARM64_X86_64_TRANSLATION table in translations.h
; points at the same bytes.
;
; The translations are per arm64 instruction form, not per macOS version,
; so they are universal across the Apple Silicon arm64 macOS builds in the
; Golden Gate series (and any other Apple Silicon macOS built on these
; forms).  Each snippet is the x86_64 code that executes where the arm64
; form does; branch-form snippets carry a zero rel32 that the consumer
; patches to the translated destination, as Stela's relocation machinery
; does.
;
; Register convention for the universal snippets: the OC_ARM64_X86_64_REG_MAP
; table in translations.h maps arm64 x0..x13 and sp onto the host
; registers used below (rax, rbx, rcx, rdx, rsi, rdi, r8..r15, rsp).
; ---------------------------------------------------------------------------

section .text

BITS 64

; --- arm64: nop (0xD503201F)  ->  x86_64: nop
mTransArm64Nop:
    nop

; --- arm64: ret (0xD65F03C0)  ->  x86_64: ret
mTransArm64Ret:
    ret

; --- arm64: b <imm26> (0x14000000 | imm26)  ->  x86_64: jmp rel32 (E9 rel32)
mTransArm64B:
    jmp near mTransArm64B_Dest
mTransArm64B_Dest:

; --- arm64: bl <imm26> (0x94000000 | imm26)  ->  x86_64: call rel32 (E8 rel32)
mTransArm64BL:
    call near mTransArm64BL_Dest
mTransArm64BL_Dest:

; --- arm64: movz x0, #imm16, LSL #0 (0xD2800000 | imm16<<5 | 0)
;             ->  x86_64: movabs rax, imm64 (48 B8 + imm64, consumer sets imm64)
mTransArm64MovzX0:
    movabs rax, strict qword 0

; --- arm64: mov x0, x1 (orr x0, xzr, x1; 0xAA0103E0)
;             ->  x86_64: mov rax, rbx (48 89 D8)
mTransArm64MovX0X1:
    mov rax, rbx

; --- arm64: ldr x0, [sp, #0x10] (0xF9C003E0? form: ldr x0, [sp, #imm])
;             ->  x86_64: mov rax, qword [rsp + 0x10]
mTransArm64LdrX0Sp:
    mov rax, qword [rsp + 0x10]

; --- arm64: str x0, [sp, #0x10] (0xF80043E0? form: str x0, [sp, #imm])
;             ->  x86_64: mov qword [rsp + 0x10], rax
mTransArm64StrX0Sp:
    mov qword [rsp + 0x10], rax

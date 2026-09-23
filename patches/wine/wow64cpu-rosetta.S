/*
 * New code for Wine 11.0's wow64cpu.dll (Gcenx wine-stable-11.0_1), placed in
 * the zero padding at the end of .text by patch-wow64cpu.py.  Everything here
 * is RIP-relative or written at run time, so the DLL's own relocation (Wine
 * maps it at 0x7bf20000, not its preferred 0x7a400000) needs no new fixups.
 *
 * Why: under Rosetta 2 the 32<->64-bit mode switch in Wine's WoW64 thunks
 * (`jmp far [ptr]` in, `ljmp *(%r14)` out) sometimes lands in the wrong mode
 * while Rosetta is discarding translated code on another thread.  CodeWeavers
 * fixed this in CrossOver ("CW HACK 20760": `lcall` in, `lretq` out); Gcenx's
 * 11.0_1 build dropped that patch.  This restores it, and additionally checks
 * the mode after every crossing and redoes the crossing if Rosetta still got
 * it wrong, counting each redo in .data (RVA 0x2ff0 in, 0x2ff4 unix-call in,
 * 0x2ff8 out).
 *
 * The mode check is one byte sequence that each mode decodes differently:
 *     31 c9      xor ecx,ecx
 *     41         32-bit: inc ecx               64-bit: REX prefix
 *     e3 NN      32-bit: jecxz, not taken      64-bit: jrcxz, taken
 */

        .text
        .code64

/*
 * This block is placed at RVA 0x1a00.  Every target is defined relative to
 * its first byte, so the assembler resolves each RIP-relative reference itself
 * and the object carries no relocations at all.
 */
code_base:
        .set RVA_BASE,          0x1a00
        .set SYSCALL_32TO64,    code_base - RVA_BASE + 0x1110
        .set UNIX_CALL_32TO64,  code_base - RVA_BASE + 0x1214
        .set CS64_SEL,          code_base - RVA_BASE + 0x6014
        .set THUNK_SYSCALL,     code_base - RVA_BASE + 0x7020
        .set THUNK_UNIXCALL,    code_base - RVA_BASE + 0x7060
        .set CNT_SYSCALL,       code_base - RVA_BASE + 0x2ff0
        .set CNT_UNIXCALL,      code_base - RVA_BASE + 0x2ff4
        .set CNT_RETURN,        code_base - RVA_BASE + 0x2ff8

/*
 * Called from BTCpuProcessInit in place of `lea 0x34(%rsp),%rax`, after
 * cs64_sel is known and before the thunk page is made PAGE_EXECUTE_READ.
 * Clobbers only rax, rdx, r10, r11, xmm0 and flags.
 */
        .globl write_thunks
write_thunks:
        lea     THUNK_SYSCALL(%rip), %r10
        lea     SYSCALL_32TO64(%rip), %r11
        lea     CNT_SYSCALL(%rip), %rdx
        call    fill_thunk
        lea     THUNK_UNIXCALL(%rip), %r10
        lea     UNIX_CALL_32TO64(%rip), %r11
        lea     CNT_UNIXCALL(%rip), %rdx
        call    fill_thunk
        lea     0x34+8(%rsp), %rax      /* the instruction this call replaced */
        ret

/* r10 = thunk, r11 = 64-bit landing pad, rdx = redo counter */
fill_thunk:
        movups  thunk_template(%rip), %xmm0
        movups  %xmm0, (%r10)
        movups  thunk_template+16(%rip), %xmm0
        movups  %xmm0, 16(%r10)
        movups  thunk_template+30(%rip), %xmm0
        movups  %xmm0, 30(%r10)
        lea     6(%r10), %rax
        mov     %eax, 2(%r10)           /* lcall operand -> far pointer */
        lea     12(%r10), %rax
        mov     %eax, 6(%r10)           /* far pointer offset -> 64-bit continuation */
        movzwl  CS64_SEL(%rip), %eax
        mov     %ax, 10(%r10)           /* far pointer selector = cs64 */
        mov     %edx, 23(%r10)          /* counter address for the 32-bit redo path */
        mov     %r11, 38(%r10)          /* where the 64-bit path goes */
        ret

/* 46 bytes; entered in 32-bit mode by the 32-bit ntdll. */
thunk_template:
        .byte   0xff, 0x1d, 0, 0, 0, 0  /* +0   lcall *[+6]                          */
        .long   0                       /* +6   far pointer offset (+12)            */
        .word   0                       /* +10  far pointer selector (cs64)         */
        .byte   0x83, 0xc4, 0x08        /* +12  add $8,%esp  (drop lcall's frame)   */
        .byte   0x31, 0xc9              /* +15  xor ecx,ecx                          */
        .byte   0x41, 0xe3, 0x0c        /* +17  [32] inc ecx;jecxz  [64] jrcxz +32   */
        .byte   0xf0, 0xff, 0x05, 0, 0, 0, 0  /* +20 [32] lock incl [counter]       */
        .byte   0xe9                    /* +27  [32] jmp +0: redo the lcall          */
        .long   -32
        .byte   0xff, 0x25, 0, 0, 0, 0  /* +32  [64] jmp *0(%rip)                    */
        .quad   0                       /* +38  landing pad                          */

/*
 * Both landing pads' fast return (`... xchg %r14,%rsp; ljmp *(%r14)`) jumps
 * here instead.  The far pointer is still built on the 64-bit stack, but it
 * now points at t32, the real EIP travels in edx, and the crossing is made
 * with lretq (CW HACK 20760).
 */
        .globl newret
newret:
        lea     t32(%rip), %rdx
        mov     %edx, (%rsp)            /* far pointer offset = t32 */
        mov     0xbc(%r13), %edx        /* context->SegCs */
        mov     %edx, 4(%rsp)
        mov     0xc4(%r13), %r14d       /* context->Esp */
        xchg    %r14, %rsp
        sub     $0x10, %rsp
        mov     4(%r14), %edx
        mov     %rdx, 8(%rsp)
        mov     0(%r14), %edx
        mov     %rdx, (%rsp)
        mov     0xb8(%r13), %edx        /* context->Eip, consumed by t32 */
        lretq

/* Where every fast return lands; edx = the real return address. */
t32:
        .byte   0x31, 0xc9              /* xor ecx,ecx                               */
        .byte   0x41, 0xe3, 0x02        /* [32] inc ecx;jecxz  [64] jrcxz -> redo    */
        .byte   0xff, 0xe2              /* [32] jmp *%edx                            */
        /* [64] Rosetta left us in 64-bit mode: count it and redo the lretq. */
        lock incl CNT_RETURN(%rip)
        sub     $0x10, %rsp
        mov     4(%r14), %ecx
        mov     %rcx, 8(%rsp)
        mov     0(%r14), %ecx
        mov     %rcx, (%rsp)
        lretq

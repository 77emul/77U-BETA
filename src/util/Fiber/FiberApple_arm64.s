// src/util/Fiber/FiberApple_arm64.s
// Native Apple Silicon ARM64 assembly context switch for Cemu Fiber.
// Compatible with Apple ARM64 ABI (AAPCS64), preserving callee-saved registers
// (x19-x28, x29/FP, x30/LR, d8-d15, FPCR, FPSR) and strictly maintaining 16-byte SP alignment.
// Does NOT touch Apple OS-reserved platform register x18.

.text
.align 4
.globl _cemu_fiber_switch
.globl _cemu_fiber_entry_trampoline
.globl _cemu_fiber_test_register_integrity

// void cemu_fiber_switch(void** from_sp, void* to_sp);
// x0 = pointer to storage for outgoing SP (from_sp)
// x1 = incoming SP (to_sp)
_cemu_fiber_switch:
    cbz x0, 1f
    cbz x1, 1f

    // Allocate 176 bytes on current stack (11 pairs of 8-byte registers, 16-byte aligned)
    sub sp, sp, #176

    // Save general-purpose callee-saved registers
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]

    // Save floating-point/SIMD callee-saved registers (lower 64 bits)
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]

    // Save floating-point control and status registers
    mrs x2, fpcr
    mrs x3, fpsr
    stp x2,  x3,  [sp, #160]

    // Save current SP into *from_sp
    mov x2, sp
    str x2, [x0]

    // Switch SP to incoming to_sp
    mov sp, x1

    // Restore floating-point control and status registers
    ldp x2,  x3,  [sp, #160]
    msr fpcr, x2
    msr fpsr, x3

    // Restore floating-point/SIMD callee-saved registers
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8,  d9,  [sp, #96]

    // Restore general-purpose callee-saved registers
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp, #0]

    // Deallocate stack frame
    add sp, sp, #176

    // Return to LR (either continuation point after previous Switch, or _cemu_fiber_entry_trampoline)
1:
    ret

// First-entry trampoline for newly created secondary Fibers
// Upon entry: x19 = FiberEntryPoint, x20 = userParam
_cemu_fiber_entry_trampoline:
    mov x0, x20         // arg0 = userParam
    blr x19             // call FiberEntryPoint(userParam)
    // Cemu Fiber entry points are infinite execution loops (e.g. __OSFiberThreadEntry, __OSThreadCoreIdle)
    // If a fiber callback unexpectedly returns, immediately trigger a clean debug trap
    brk #0x1

// Dedicated ABI preservation validation helper
// bool cemu_fiber_test_register_integrity(void (*switch_fn)(void*), void* context);
// Saves original caller registers, pre-fills callee-saved registers with signature patterns,
// invokes switch_fn(context), checks all registers upon resumption, and returns 1 (PASS) or 0 (FAIL).
_cemu_fiber_test_register_integrity:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x2, fpcr
    mrs x3, fpsr
    stp x2,  x3,  [sp, #160]

    // Pre-fill integer callee-saved registers with distinct signatures
    movz x19, #0x1919, lsl #48
    movk x19, #0x1919, lsl #32
    movk x19, #0x1919, lsl #16
    movk x19, #0x1919

    movz x20, #0x2020, lsl #48
    movk x20, #0x2020, lsl #32
    movk x20, #0x2020, lsl #16
    movk x20, #0x2020

    movz x21, #0x2121, lsl #48
    movz x22, #0x2222, lsl #48
    movz x23, #0x2323, lsl #48
    movz x24, #0x2424, lsl #48
    movz x25, #0x2525, lsl #48
    movz x26, #0x2626, lsl #48
    movz x27, #0x2727, lsl #48
    movz x28, #0x2828, lsl #48

    // Pre-fill FP callee-saved registers with distinct floating-point constants
    adrp x2, k_fiber_fp_constants@PAGE
    add  x2, x2, k_fiber_fp_constants@PAGEOFF
    ldp  d8,  d9,  [x2, #0]
    ldp  d10, d11, [x2, #16]
    ldp  d12, d13, [x2, #32]
    ldp  d14, d15, [x2, #48]

    // Invoke switch_fn(context)
    mov x2, x0          // x2 = switch_fn pointer
    mov x0, x1          // x0 = context pointer
    blr x2

    // Check all integer callee-saved registers
    mov x0, #1

    movz x2, #0x1919, lsl #48
    movk x2, #0x1919, lsl #32
    movk x2, #0x1919, lsl #16
    movk x2, #0x1919
    cmp x19, x2
    b.ne 1f

    movz x2, #0x2020, lsl #48
    movk x2, #0x2020, lsl #32
    movk x2, #0x2020, lsl #16
    movk x2, #0x2020
    cmp x20, x2
    b.ne 1f

    movz x2, #0x2121, lsl #48
    cmp x21, x2
    b.ne 1f
    movz x2, #0x2222, lsl #48
    cmp x22, x2
    b.ne 1f
    movz x2, #0x2323, lsl #48
    cmp x23, x2
    b.ne 1f
    movz x2, #0x2424, lsl #48
    cmp x24, x2
    b.ne 1f
    movz x2, #0x2525, lsl #48
    cmp x25, x2
    b.ne 1f
    movz x2, #0x2626, lsl #48
    cmp x26, x2
    b.ne 1f
    movz x2, #0x2727, lsl #48
    cmp x27, x2
    b.ne 1f
    movz x2, #0x2828, lsl #48
    cmp x28, x2
    b.ne 1f

    // Check all floating-point callee-saved registers
    adrp x2, k_fiber_fp_constants@PAGE
    add  x2, x2, k_fiber_fp_constants@PAGEOFF
    ldr  d0, [x2, #0]
    fcmp d8, d0
    b.ne 1f
    ldr  d0, [x2, #8]
    fcmp d9, d0
    b.ne 1f
    ldr  d0, [x2, #16]
    fcmp d10, d0
    b.ne 1f
    ldr  d0, [x2, #24]
    fcmp d11, d0
    b.ne 1f
    ldr  d0, [x2, #32]
    fcmp d12, d0
    b.ne 1f
    ldr  d0, [x2, #40]
    fcmp d13, d0
    b.ne 1f
    ldr  d0, [x2, #48]
    fcmp d14, d0
    b.ne 1f
    ldr  d0, [x2, #56]
    fcmp d15, d0
    b.ne 1f

    b 2f

1:  mov x0, #0

2:  // Restore original caller context
    ldp x2,  x3,  [sp, #160]
    msr fpcr, x2
    msr fpsr, x3
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8,  d9,  [sp, #96]
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp, #0]
    add sp, sp, #176
    ret

.section __TEXT,__const
.align 4
k_fiber_fp_constants:
    .double 108.0, 109.0, 110.0, 111.0, 112.0, 113.0, 114.0, 115.0

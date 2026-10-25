# SPDX-License-Identifier: GPL-3.0-or-later
# Native Xenon loader -> PPC32 compiler boundary. Preserve full nonvolatile GPRs,
# LR and CR2-4 across the DllMain callback. Same proven contract as the trainer.
    .text
    .align 2
    .globl UnifyEntry
UnifyEntry:
    mflr 0
    std 0,16(1)
    stdu 1,-288(1)
    mfcr 12
    stw 12,80(1)
    std 14,128(1)
    std 15,136(1)
    std 16,144(1)
    std 17,152(1)
    std 18,160(1)
    std 19,168(1)
    std 20,176(1)
    std 21,184(1)
    std 22,192(1)
    std 23,200(1)
    std 24,208(1)
    std 25,216(1)
    std 26,224(1)
    std 27,232(1)
    std 28,240(1)
    std 29,248(1)
    std 30,256(1)
    std 31,264(1)
    bl UnifyEntryCpp
    ld 14,128(1)
    ld 15,136(1)
    ld 16,144(1)
    ld 17,152(1)
    ld 18,160(1)
    ld 19,168(1)
    ld 20,176(1)
    ld 21,184(1)
    ld 22,192(1)
    ld 23,200(1)
    ld 24,208(1)
    ld 25,216(1)
    ld 26,224(1)
    ld 27,232(1)
    ld 28,240(1)
    ld 29,248(1)
    ld 30,256(1)
    ld 31,264(1)
    lwz 12,80(1)
    mtcrf 56,12
    addi 1,1,288
    ld 0,16(1)
    mtlr 0
    blr

# unsigned long long UnifyCallGate(void* fn, const unsigned int* args8)
# loads r3..r10 from args8[0..7] (32-bit each, zero extended), calls fn via ctr,
# returns the full 64-bit r3. the caller zero-fills unused arg slots, so this needs
# no per-argc branching. only integer/pointer args; float args are a later addition.
    .globl UnifyCallGate
UnifyCallGate:
    mflr 0
    std 0,16(1)
    stdu 1,-112(1)
    mr 11,3            # fn
    mr 12,4            # args8
    lwz 3,0(12)
    lwz 4,4(12)
    lwz 5,8(12)
    lwz 6,12(12)
    lwz 7,16(12)
    lwz 8,20(12)
    lwz 9,24(12)
    lwz 10,28(12)
    mtctr 11
    bctrl
    addi 1,1,112
    ld 0,16(1)
    mtlr 0
    blr

# unsigned long long UnifyCallGateF(void* fn, const unsigned int* gpr8,
#                                   const double* fpr8, double* fret)
# loads r3..r10 from gpr8[0..7] AND f1..f8 from fpr8[0..7], calls fn, stores the
# float result f1 into *fret, returns the integer result r3. this covers mixed
# int/float signatures: every arg takes its gpr slot (r3+i), and float args also
# occupy f1..fN in order, per the xbox 360 abi. the caller zero-fills unused slots.
    .globl UnifyCallGateF
UnifyCallGateF:
    mflr 0
    std 0,16(1)
    stdu 1,-176(1)
    std 6,160(1)       # save fret
    mr 11,3            # fn
    mr 12,4            # gpr8
    lfd 1,0(5)
    lfd 2,8(5)
    lfd 3,16(5)
    lfd 4,24(5)
    lfd 5,32(5)
    lfd 6,40(5)
    lfd 7,48(5)
    lfd 8,56(5)
    lwz 3,0(12)
    lwz 4,4(12)
    lwz 5,8(12)
    lwz 6,12(12)
    lwz 7,16(12)
    lwz 8,20(12)
    lwz 9,24(12)
    lwz 10,28(12)
    mtctr 11
    bctrl
    ld 12,160(1)       # restore fret
    stfd 1,0(12)       # *fret = f1
    addi 1,1,176
    ld 0,16(1)
    mtlr 0
    blr

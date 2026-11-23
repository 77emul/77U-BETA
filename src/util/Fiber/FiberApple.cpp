// src/util/Fiber/FiberApple.cpp
// Native Apple Silicon ARM64 Fiber implementation for Cemu.
// Reproduces exact Fiber class contract with standard 2 MB stack,
// 16-byte aligned context frames, FPCR/FPSR preservation, and zero POSIX ucontext dependencies.

#include "Fiber.h"
#include <cstdlib>
#include <atomic>
#include "Common/precompiled.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"

extern "C" {
    void cemu_fiber_switch(void** from_sp, void* to_sp);
    void cemu_fiber_entry_trampoline();
}

thread_local Fiber* sCurrentFiber{};

struct FiberContextApple {
    void* sp{nullptr};
};

static inline uint64_t Fiber_GetCurrentFPCR() {
    uint64_t val;
    asm volatile("mrs %0, fpcr" : "=r"(val));
    return val;
}

static inline uint64_t Fiber_GetCurrentFPSR() {
    uint64_t val;
    asm volatile("mrs %0, fpsr" : "=r"(val));
    return val;
}

// Secondary Fiber constructor
Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) 
    : m_privateData(privateData)
{
    const size_t stackSize = 2 * 1024 * 1024; // 2 MB stack, matching FiberUnix & FiberWin
    m_stackPtr = malloc(stackSize);
    cemu_assert(m_stackPtr != nullptr);

    uintptr_t stackTop = (uintptr_t)m_stackPtr + stackSize;
    stackTop &= ~0xFULL; // 16-byte alignment

    // Allocate 176-byte context frame on the new stack
    stackTop -= 176;
    uint64_t* frame = (uint64_t*)stackTop;

    // x19 = FiberEntryPoint function pointer
    frame[0] = (uint64_t)FiberEntryPoint;
    // x20 = userParam
    frame[1] = (uint64_t)userParam;
    // x21..x28 = 0 (callee-saved general-purpose registers)
    for (int i = 2; i <= 9; ++i) frame[i] = 0;
    // x29 (FP) = 0 (terminates frame backtrace cleanly)
    frame[10] = 0;
    // x30 (LR) = _cemu_fiber_entry_trampoline
    frame[11] = (uint64_t)cemu_fiber_entry_trampoline;
    // d8..d15 = 0 (callee-saved floating-point registers)
    for (int i = 12; i <= 19; ++i) frame[i] = 0;
    // FPCR / FPSR: inherit current host thread floating-point control & status
    frame[20] = Fiber_GetCurrentFPCR();
    frame[21] = Fiber_GetCurrentFPSR();

    FiberContextApple* ctx = (FiberContextApple*)malloc(sizeof(FiberContextApple));
    cemu_assert(ctx != nullptr);
    ctx->sp = (void*)stackTop;
    this->m_implData = (void*)ctx;
}

// Main thread Fiber constructor (called by PrepareCurrentThread)
Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
    FiberContextApple* ctx = (FiberContextApple*)malloc(sizeof(FiberContextApple));
    cemu_assert(ctx != nullptr);
    ctx->sp = nullptr; // Captured on first outgoing switch
    this->m_implData = (void*)ctx;
    this->m_stackPtr = nullptr; // Main thread owns its host stack
}

Fiber::~Fiber()
{
    if (m_stackPtr)
        free(m_stackPtr);
    free(m_implData);
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
    cemu_assert_debug(sCurrentFiber == nullptr);
    ExceptionHandler_InstallAltStack();
    sCurrentFiber = new Fiber(privateData);
    return sCurrentFiber;
}

void Fiber::Switch(Fiber& targetFiber)
{
    Fiber* leavingFiber = sCurrentFiber;
    if (!leavingFiber || leavingFiber == &targetFiber)
        return;
    sCurrentFiber = &targetFiber;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    cemu_fiber_switch(&(((FiberContextApple*)leavingFiber->m_implData)->sp),
                      ((FiberContextApple*)targetFiber.m_implData)->sp);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void* Fiber::GetFiberPrivateData()
{
    return sCurrentFiber ? sCurrentFiber->m_privateData : nullptr;
}

void Fiber::SetFiberPrivateData(void* privateData)
{
    if (sCurrentFiber)
        sCurrentFiber->m_privateData = privateData;
}

src = r'''
// ============================================================
// VEH + DR0 Hardware Breakpoint (anti-unit-cull, zero .text write)
// ============================================================
// Strategy:
//   - AddVectoredExceptionHandler catches EXCEPTION_SINGLE_STEP
//   - Set DR0 via SetThreadContext for execution breakpoint on target
//   - In VEH: force EAX=1, pop EIP from stack (simulated RET), ESP+=4
//   - Memory is never modified: page attributes remain PAGE_EXECUTE_READ
//   - Battle.net Code Integrity Scan sees original bytes

static PVOID g_vehHandle = NULL;
static volatile LONG g_dr0Active = 0;
static DWORD g_dr0Target = 0;

// Set DR0 via a temporary debug context (must be called from game thread)
// We use GetThreadContext/SetThreadContext on the current thread.
static void SetDR0ForCurrentThread(DWORD addr) {
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_DEBUG_CONTROL;
    GetThreadContext(GetCurrentThread(), &ctx);
    ctx.Dr0 = addr;
    ctx.Dr6 &= ~0x10;  // clear B0 status
    ctx.Dr7 &= ~(1 << 16);  // clear B0 enable (bit 16 in DR7)
    ctx.Dr7 |= (1 << 16) | (1 << 0);  // enable B0, local enable
    ctx.Dr7 |= (1 << 18);  // D0: execution (01 << 16*2 = bits 18-17)
    // Wait: DR7 layout:
    //   bits 0-7: L0-L7 (local enable), G0-G7 (global enable)  
    //   bits 16-23: L0', L1', L2', L3' (for local)
    //   bits 16-31: R/W0-R/W7, E0-E7
    //   Actually: for D0:
    //     LE0 = bit 0, GE0 = bit 1
    //     R/W0 = bits 16-17 (01=execution)
    //     LEN0 = bits 18-19 (00=1 byte)
    //     END0 = bit 20, GEND0 = bit 21
    // Let me redo:
    // DR7 bit 0: LE0 (local enable D0)
    // DR7 bit 1: GE0 (global enable D0)  
    // DR7 bits 16-17: R/W0 (00=read, 01=write, 10=IO, 11=execution)
    // DR7 bits 18-19: LEN0 (00=1byte, 01=2, 10=4, 11=8)
    // So for execution breakpoint on D0:
    //   DR7 |= (1 << 0)       // LE0
    //   DR7 |= (3 << 16)      // R/W0 = 11 (execution)
    // That's it.
    
    ctx = (CONTEXT){0};
    ctx.ContextFlags = CONTEXT_DEBUG_CONTROL;
    GetThreadContext(GetCurrentThread(), &ctx);
    ctx.Dr0 = addr;
    ctx.Dr7 = (ctx.Dr7 & ~0x00000003) | 0x00000001;  // LE0=1, GE0=0
    ctx.Dr7 |= (3 << 16);  // R/W0 = execution
    ctx.Dr7 &= ~(3 << 18); // LEN0 = 1 byte
    SetThreadContext(GetCurrentThread(), &ctx);
}

// Clear DR0
static void ClearDR0ForCurrentThread() {
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_DEBUG_CONTROL;
    GetThreadContext(GetCurrentThread(), &ctx);
    ctx.Dr0 = 0;
    ctx.Dr7 &= ~0x00000003;  // clear LE0/GE0
    SetThreadContext(GetCurrentThread(), &ctx);
}

static LONG VEH_Handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        if (g_dr0Active && ep->ExceptionRecord->ExceptionAddress == (PVOID)g_dr0Target) {
            CONTEXT* ctx = ep->ContextRecord;
            // Set EAX = 1 (function returns TRUE/"visible")
            ctx->Eax = 1;
            // Simulate RET: pop return address from stack
            // EIP should be at the instruction AFTER the call, 
            // but we're intercepting the function entry, so:
            // The stack has: [return_addr] [saved_esp] [args...]
            // We pop return_addr and set EIP, then adjust ESP
            void** stackTop = (void**)(ctx->Esp);
            DWORD retAddr = (DWORD)stackTop[0];
            ctx->Eip = retAddr;
            ctx->Esp += 4;
            // Clear DR0 active flag (re-enable on next call if needed)
            InterlockedExchange(&g_dr0Active, 0);
            ClearDR0ForCurrentThread();
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Public API: enable hardware breakpoint at target address
// Must be called from the game's main/render thread
void VEH_EnableDR0(DWORD targetAddr) {
    if (!g_vehHandle) {
        g_vehHandle = AddVectoredExceptionHandler(1, VEH_Handler);
        Fog_Log("VEH handler installed at %p\n", g_vehHandle);
    }
    g_dr0Target = targetAddr;
    SetDR0ForCurrentThread(targetAddr);
    InterlockedExchange(&g_dr0Active, 1);
    Fog_Log("DR0 execution breakpoint set at 0x%08X (zero memory write)\n", targetAddr);
}

void VEH_DisableDR0() {
    InterlockedExchange(&g_dr0Active, 0);
    ClearDR0ForCurrentThread();
    Fog_Log("DR0 breakpoint cleared\n");
}
'''
with open(r"G:\LocalCodex\CodexSessions\2026-11-11_04-38-46_a628a9d6\ra3fog\src\d3d9proxy.cpp", "a", encoding="utf-8") as f:
    f.write(src)
print(f"Part 3: {len(src)} chars appended")

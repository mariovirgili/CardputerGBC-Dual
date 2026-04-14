#include "coleco_cpu.h"

#include <cstdio>
#include <cstring>
#include "../coleco_media.h"
#include "coleco_vdp.h"

#ifndef COLECO_CPU_TRACE_ENABLED
#define COLECO_CPU_TRACE_ENABLED 0
#endif

namespace {

constexpr uint8_t kColecoPrimarySlotCartridge = 1u;
constexpr uint8_t kColecoOpenBusFetchOpcodeRet = 0xC9u;
constexpr uint8_t kColecoBootSlotCart = 0xD4u;
constexpr uint8_t kColecoBootSecondaryCart = 0xA0u;

constexpr uint8_t kFlagC = 0x01;
constexpr uint8_t kFlagN = 0x02;
constexpr uint8_t kFlagPV = 0x04;
constexpr uint8_t kFlagX = 0x08;
constexpr uint8_t kFlagH = 0x10;
constexpr uint8_t kFlagY = 0x20;
constexpr uint8_t kFlagZ = 0x40;
constexpr uint8_t kFlagS = 0x80;

inline uint8_t coleco_hi(uint16_t value)
{
    return static_cast<uint8_t>(value >> 8);
}

inline uint8_t coleco_lo(uint16_t value)
{
    return static_cast<uint8_t>(value & 0x00FFu);
}

inline void coleco_set_hi(uint16_t* value, uint8_t hi)
{
    *value = static_cast<uint16_t>((*value & 0x00FFu) | (static_cast<uint16_t>(hi) << 8));
}

inline void coleco_set_lo(uint16_t* value, uint8_t lo)
{
    *value = static_cast<uint16_t>((*value & 0xFF00u) | lo);
}

inline uint8_t coleco_cpu_a(const ColecoCpuState* state)
{
    return coleco_hi(state->af);
}

inline uint8_t coleco_cpu_f(const ColecoCpuState* state)
{
    return coleco_lo(state->af);
}

inline void coleco_cpu_set_a(ColecoCpuState* state, uint8_t value)
{
    coleco_set_hi(&state->af, value);
}

inline void coleco_cpu_set_f(ColecoCpuState* state, uint8_t value)
{
    coleco_set_lo(&state->af, value);
}

inline bool coleco_parity_even(uint8_t value)
{
    value ^= static_cast<uint8_t>(value >> 4);
    value &= 0x0Fu;
    return ((0x6996u >> value) & 0x01u) == 0;
}

inline uint8_t coleco_flags_szxy(uint8_t value)
{
    uint8_t flags = static_cast<uint8_t>(value & (kFlagS | kFlagX | kFlagY));
    if (value == 0) {
        flags |= kFlagZ;
    }
    return flags;
}

inline uint8_t coleco_flags_szpxy(uint8_t value)
{
    uint8_t flags = coleco_flags_szxy(value);
    if (coleco_parity_even(value)) {
        flags |= kFlagPV;
    }
    return flags;
}

inline int8_t coleco_signed_offset(uint8_t value)
{
    return static_cast<int8_t>(value);
}

#if COLECO_CPU_TRACE_ENABLED
inline bool coleco_cpu_trace_pc(uint16_t pc)
{
    return ((pc >= 0x3F18u) && (pc <= 0x3F30u)) ||
           ((pc >= 0x4100u) && (pc <= 0x423Fu)) ||
           ((pc >= 0x7D0Du) && (pc <= 0x7D10u)) ||
           ((pc >= 0x8170u) && (pc <= 0x8190u)) ||
           (pc == 0xFD9Au);
}

inline bool coleco_cpu_trace_opcode_pc(uint16_t pc)
{
    return ((pc >= 0x3F18u) && (pc <= 0x3F30u)) ||
           ((pc >= 0x4100u) && (pc <= 0x423Fu)) ||
           ((pc >= 0x7D0Du) && (pc <= 0x7D10u)) ||
           ((pc >= 0x8170u) && (pc <= 0x8190u)) ||
           (pc == 0xFD9Au);
}

inline bool coleco_cpu_trace_addr(uint16_t address)
{
    return (address >= 0xEFF0u) && (address <= 0xF020u);
}

inline bool coleco_cpu_trace_cart_probe_addr(uint16_t address)
{
    return (address <= 0x0007u) ||
           ((address >= 0x4000u) && (address <= 0x4007u)) ||
           ((address >= 0x8000u) && (address <= 0x8007u));
}

inline bool coleco_cpu_trace_workarea_addr(uint16_t address)
{
    return (address >= 0xF7C5u) && (address <= 0xF7C8u);
}

inline bool coleco_cpu_trace_context_pc(uint16_t pc)
{
    return (pc == 0x0038u) ||
           (pc == 0x3F18u) ||
           (pc == 0x3F23u) ||
           (pc == 0x3F24u) ||
           (pc == 0x410Cu) ||
           (pc == 0x4120u) ||
           (pc == 0x41D1u) ||
           (pc == 0x41D4u) ||
           (pc == 0x7D0Du) ||
           (pc == 0x7D10u) ||
           (pc == 0x8132u) ||
           (pc == 0x8175u) ||
           (pc == 0xFD9Au);
}

inline bool coleco_cpu_trace_stack_value(uint16_t value)
{
    return ((value >= 0x3F18u) && (value <= 0x3F30u)) ||
           ((value >= 0x4100u) && (value <= 0x423Fu)) ||
           ((value >= 0x7D0Du) && (value <= 0x7D10u)) ||
           ((value >= 0x8170u) && (value <= 0x8190u)) ||
           (value == 0x0038u) ||
           (value == 0xFD9Au);
}
#else
inline bool coleco_cpu_trace_pc(uint16_t)
{
    return false;
}

inline bool coleco_cpu_trace_opcode_pc(uint16_t)
{
    return false;
}

inline bool coleco_cpu_trace_addr(uint16_t)
{
    return false;
}

inline bool coleco_cpu_trace_cart_probe_addr(uint16_t)
{
    return false;
}

inline bool coleco_cpu_trace_workarea_addr(uint16_t)
{
    return false;
}

inline bool coleco_cpu_trace_context_pc(uint16_t)
{
    return false;
}

inline bool coleco_cpu_trace_stack_value(uint16_t)
{
    return false;
}
#endif

inline uint8_t coleco_cpu_vdp_reg1(const ColecoMemoryState* memory)
{
    return (memory && memory->vdp) ? memory->vdp->regs[1] : 0xFFu;
}









void coleco_cpu_log_context(const ColecoCpuState* state, const ColecoMemoryState* memory, uint16_t pc)
{
    if (!state || !memory || !coleco_cpu_trace_context_pc(pc)) {
        return;
    }

    static uint16_t s_contextLogCount = 0u;
    if (s_contextLogCount >= 128u) {
        return;
    }

    std::printf("[MSX][CTX] pc=%04X af=%04X bc=%04X de=%04X hl=%04X sp=%04X iff=%u irq=%u halt=%u r1=%02X a8=%02X ssl3=%02X #%u\n",
                static_cast<unsigned>(pc),
                static_cast<unsigned>(state->af),
                static_cast<unsigned>(state->bc),
                static_cast<unsigned>(state->de),
                static_cast<unsigned>(state->hl),
                static_cast<unsigned>(state->sp),
                state->iff1 ? 1u : 0u,
                state->irqPending ? 1u : 0u,
                state->halted ? 1u : 0u,
                static_cast<unsigned>(coleco_cpu_vdp_reg1(memory)),
                0,
                0,
                static_cast<unsigned>(s_contextLogCount));
    ++s_contextLogCount;
}

void coleco_cpu_log_flow(const char* kind,
                      const ColecoCpuState* state,
                      const ColecoMemoryState* memory,
                      uint16_t from,
                      uint16_t to)
{
    if (!kind || !state || !memory) {
        return;
    }

    if (!coleco_cpu_trace_pc(from) && !coleco_cpu_trace_pc(to)) {
        return;
    }

    static uint16_t s_flowLogCount = 0u;
    if (s_flowLogCount >= 256u) {
        return;
    }

    std::printf("[MSX][FLOW] %s %04X -> %04X sp=%04X iff=%u irq=%u halt=%u r1=%02X a8=%02X ssl3=%02X #%u\n",
                kind,
                static_cast<unsigned>(from),
                static_cast<unsigned>(to),
                static_cast<unsigned>(state->sp),
                state->iff1 ? 1u : 0u,
                state->irqPending ? 1u : 0u,
                state->halted ? 1u : 0u,
                static_cast<unsigned>(coleco_cpu_vdp_reg1(memory)),
                0,
                0,
                static_cast<unsigned>(s_flowLogCount));
    ++s_flowLogCount;
}

void coleco_cpu_log_stack(const char* kind,
                       const ColecoCpuState* state,
                       const ColecoMemoryState* memory,
                       uint16_t pc,
                       uint16_t spBefore,
                       uint16_t value)
{
    if (!kind || !state || !memory) {
        return;
    }

    if (!coleco_cpu_trace_pc(pc) &&
        !coleco_cpu_trace_addr(spBefore) &&
        !coleco_cpu_trace_stack_value(value)) {
        return;
    }

    static uint16_t s_stackLogCount = 0u;
    if (s_stackLogCount >= 128u) {
        return;
    }

    std::printf("[MSX][STACK] %s pc=%04X sp=%04X value=%04X iff=%u irq=%u halt=%u r1=%02X a8=%02X ssl3=%02X #%u\n",
                kind,
                static_cast<unsigned>(pc),
                static_cast<unsigned>(spBefore),
                static_cast<unsigned>(value),
                state->iff1 ? 1u : 0u,
                state->irqPending ? 1u : 0u,
                state->halted ? 1u : 0u,
                static_cast<unsigned>(coleco_cpu_vdp_reg1(memory)),
                0,
                0,
                static_cast<unsigned>(s_stackLogCount));
    ++s_stackLogCount;
}

void coleco_cpu_log_ram_access(const char* kind,
                            const ColecoMemoryState* memory,
                            uint16_t address,
                            uint8_t value)
{
    (void)kind;
    (void)memory;
    (void)address;
    (void)value;
}

void coleco_cpu_log_opcode(const ColecoCpuState* state,
                        const ColecoMemoryState* memory,
                        uint16_t pc,
                        uint8_t opcode)
{
    if (!state || !memory || !coleco_cpu_trace_opcode_pc(pc)) {
        return;
    }

    static uint16_t s_opcodeLogCount = 0u;
    if (s_opcodeLogCount >= 512u) {
        return;
    }

    const uint8_t next1 = coleco_memory_read8(memory, static_cast<uint16_t>(pc + 1u));
    const uint8_t next2 = coleco_memory_read8(memory, static_cast<uint16_t>(pc + 2u));
    std::printf("[MSX][OP] pc=%04X op=%02X n1=%02X n2=%02X af=%04X bc=%04X de=%04X hl=%04X sp=%04X iff=%u irq=%u halt=%u r1=%02X a8=%02X ssl3=%02X #%u\n",
                static_cast<unsigned>(pc),
                static_cast<unsigned>(opcode),
                static_cast<unsigned>(next1),
                static_cast<unsigned>(next2),
                static_cast<unsigned>(state->af),
                static_cast<unsigned>(state->bc),
                static_cast<unsigned>(state->de),
                static_cast<unsigned>(state->hl),
                static_cast<unsigned>(state->sp),
                state->iff1 ? 1u : 0u,
                state->irqPending ? 1u : 0u,
                state->halted ? 1u : 0u,
                static_cast<unsigned>(coleco_cpu_vdp_reg1(memory)),
                0,
                0,
                static_cast<unsigned>(s_opcodeLogCount));
    ++s_opcodeLogCount;
}

inline uint8_t coleco_cpu_mem_read8(const ColecoMemoryState* memory, uint16_t address)
{
    return coleco_memory_read8(memory, address);
}

inline bool coleco_cpu_fetch_should_return_open_bus_ret(const ColecoMemoryState* memory, uint16_t address) { return false; }



void coleco_cpu_restore_cart_boot_mapping(ColecoMemoryState* memory, uint16_t target, uint16_t fromPc) {}

inline uint16_t coleco_cpu_mem_read16(const ColecoMemoryState* memory, uint16_t address)
{
    const uint8_t lo = coleco_cpu_mem_read8(memory, address);
    const uint8_t hi = coleco_cpu_mem_read8(memory, static_cast<uint16_t>(address + 1u));
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

inline void coleco_cpu_mem_write8(ColecoMemoryState* memory, uint16_t address, uint8_t value)
{
    coleco_memory_write8(memory, address, value);
}

inline void coleco_cpu_mem_write16(ColecoMemoryState* memory, uint16_t address, uint16_t value)
{
    coleco_cpu_mem_write8(memory, address, static_cast<uint8_t>(value & 0xFFu));
    coleco_cpu_mem_write8(memory, static_cast<uint16_t>(address + 1u), static_cast<uint8_t>(value >> 8));
}

uint8_t coleco_cpu_get_reg8(const ColecoCpuState* state, const ColecoMemoryState* memory, uint8_t reg)
{
    switch (reg & 0x07u) {
        case 0: return coleco_hi(state->bc);
        case 1: return coleco_lo(state->bc);
        case 2: return coleco_hi(state->de);
        case 3: return coleco_lo(state->de);
        case 4: return coleco_hi(state->hl);
        case 5: return coleco_lo(state->hl);
        case 6: return coleco_cpu_mem_read8(memory, state->hl);
        default: return coleco_cpu_a(state);
    }
}

void coleco_cpu_set_reg8(ColecoCpuState* state, ColecoMemoryState* memory, uint8_t reg, uint8_t value)
{
    switch (reg & 0x07u) {
        case 0: coleco_set_hi(&state->bc, value); break;
        case 1: coleco_set_lo(&state->bc, value); break;
        case 2: coleco_set_hi(&state->de, value); break;
        case 3: coleco_set_lo(&state->de, value); break;
        case 4: coleco_set_hi(&state->hl, value); break;
        case 5: coleco_set_lo(&state->hl, value); break;
        case 6: coleco_cpu_mem_write8(memory, state->hl, value); break;
        default: coleco_cpu_set_a(state, value); break;
    }
}

uint16_t* coleco_cpu_reg16_ptr(ColecoCpuState* state, uint8_t pair)
{
    switch (pair & 0x03u) {
        case 0: return &state->bc;
        case 1: return &state->de;
        case 2: return &state->hl;
        default: return &state->sp;
    }
}

uint16_t* coleco_cpu_stack_reg16_ptr(ColecoCpuState* state, uint8_t pair)
{
    switch (pair & 0x03u) {
        case 0: return &state->bc;
        case 1: return &state->de;
        case 2: return &state->hl;
        default: return &state->af;
    }
}

uint16_t coleco_cpu_fetch16(ColecoCpuState* state, const ColecoMemoryState* memory);

// Forward declaration: coleco_cpu_step_xy's default case re-dispatches here
int coleco_cpu_step_opcode(ColecoCpuState* state, ColecoMemoryState* memory);

uint8_t coleco_cpu_fetch8(ColecoCpuState* state, const ColecoMemoryState* memory)
{
    uint8_t value = coleco_cpu_mem_read8(memory, state->pc);
    if (coleco_cpu_fetch_should_return_open_bus_ret(memory, state->pc)) {
        value = kColecoOpenBusFetchOpcodeRet;
    }
    state->pc = static_cast<uint16_t>(state->pc + 1u);
    state->r = static_cast<uint8_t>(state->r + 1u);
    return value;
}

uint16_t coleco_cpu_fetch16(ColecoCpuState* state, const ColecoMemoryState* memory)
{
    const uint8_t lo = coleco_cpu_fetch8(state, memory);
    const uint8_t hi = coleco_cpu_fetch8(state, memory);
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

void coleco_cpu_push16(ColecoCpuState* state, ColecoMemoryState* memory, uint16_t value)
{
    const uint16_t spBefore = state->sp;
    state->sp = static_cast<uint16_t>(state->sp - 2u);
    coleco_cpu_mem_write16(memory, state->sp, value);
    coleco_cpu_log_stack("PUSH", state, memory, state->lastPc, spBefore, value);
}

uint16_t coleco_cpu_pop16(ColecoCpuState* state, const ColecoMemoryState* memory)
{
    const uint16_t spBefore = state->sp;
    const uint16_t value = coleco_cpu_mem_read16(memory, state->sp);
    state->sp = static_cast<uint16_t>(state->sp + 2u);
    coleco_cpu_log_stack("POP", state, memory, state->lastPc, spBefore, value);
    return value;
}

bool coleco_cpu_condition(const ColecoCpuState* state, uint8_t condition)
{
    const uint8_t flags = coleco_cpu_f(state);
    switch (condition & 0x07u) {
        case 0: return (flags & kFlagZ) == 0;
        case 1: return (flags & kFlagZ) != 0;
        case 2: return (flags & kFlagC) == 0;
        case 3: return (flags & kFlagC) != 0;
        case 4: return (flags & kFlagPV) == 0;
        case 5: return (flags & kFlagPV) != 0;
        case 6: return (flags & kFlagS) == 0;
        default: return (flags & kFlagS) != 0;
    }
}

void coleco_cpu_exchange16(uint16_t* lhs, uint16_t* rhs)
{
    const uint16_t value = *lhs;
    *lhs = *rhs;
    *rhs = value;
}

uint8_t coleco_cpu_inc8(ColecoCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(value + 1u);
    uint8_t flags = static_cast<uint8_t>(coleco_cpu_f(state) & kFlagC);
    flags |= coleco_flags_szxy(result);
    if ((value & 0x0Fu) == 0x0Fu) {
        flags |= kFlagH;
    }
    if (value == 0x7Fu) {
        flags |= kFlagPV;
    }
    coleco_cpu_set_f(state, flags);
    return result;
}

uint8_t coleco_cpu_dec8(ColecoCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(value - 1u);
    uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | kFlagN);
    flags |= coleco_flags_szxy(result);
    if ((value & 0x0Fu) == 0x00u) {
        flags |= kFlagH;
    }
    if (value == 0x80u) {
        flags |= kFlagPV;
    }
    coleco_cpu_set_f(state, flags);
    return result;
}

uint8_t coleco_cpu_add8(ColecoCpuState* state, uint8_t lhs, uint8_t rhs, uint8_t carry)
{
    const uint16_t full = static_cast<uint16_t>(lhs) + static_cast<uint16_t>(rhs) + static_cast<uint16_t>(carry);
    const uint8_t result = static_cast<uint8_t>(full & 0x00FFu);
    uint8_t flags = coleco_flags_szxy(result);
    if (((lhs & 0x0Fu) + (rhs & 0x0Fu) + carry) > 0x0Fu) {
        flags |= kFlagH;
    }
    if (((~(lhs ^ rhs) & (lhs ^ result)) & 0x80u) != 0) {
        flags |= kFlagPV;
    }
    if ((full & 0x0100u) != 0) {
        flags |= kFlagC;
    }
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, flags);
    return result;
}

uint8_t coleco_cpu_sub8(ColecoCpuState* state, uint8_t lhs, uint8_t rhs, uint8_t carry)
{
    const uint16_t full = static_cast<uint16_t>(lhs) - static_cast<uint16_t>(rhs) - static_cast<uint16_t>(carry);
    const uint8_t result = static_cast<uint8_t>(full & 0x00FFu);
    uint8_t flags = static_cast<uint8_t>(coleco_flags_szxy(result) | kFlagN);
    if ((static_cast<uint16_t>(lhs & 0x0Fu) - static_cast<uint16_t>(rhs & 0x0Fu) - carry) & 0x0010u) {
        flags |= kFlagH;
    }
    if ((((lhs ^ rhs) & (lhs ^ result)) & 0x80u) != 0) {
        flags |= kFlagPV;
    }
    if ((full & 0x0100u) != 0) {
        flags |= kFlagC;
    }
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, flags);
    return result;
}

void coleco_cpu_logic_and(ColecoCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(coleco_cpu_a(state) & value);
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, static_cast<uint8_t>(coleco_flags_szpxy(result) | kFlagH));
}

void coleco_cpu_logic_xor(ColecoCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(coleco_cpu_a(state) ^ value);
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, coleco_flags_szpxy(result));
}

void coleco_cpu_logic_or(ColecoCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(coleco_cpu_a(state) | value);
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, coleco_flags_szpxy(result));
}

void coleco_cpu_compare8(ColecoCpuState* state, uint8_t value)
{
    const uint8_t accumulator = coleco_cpu_a(state);
    const uint16_t full = static_cast<uint16_t>(accumulator) - static_cast<uint16_t>(value);
    const uint8_t result = static_cast<uint8_t>(full & 0x00FFu);
    uint8_t flags = static_cast<uint8_t>((value & (kFlagX | kFlagY)) | kFlagN);
    if (result & 0x80u) {
        flags |= kFlagS;
    }
    if (result == 0) {
        flags |= kFlagZ;
    }
    if ((static_cast<uint16_t>(accumulator & 0x0Fu) - static_cast<uint16_t>(value & 0x0Fu)) & 0x0010u) {
        flags |= kFlagH;
    }
    if ((((accumulator ^ value) & (accumulator ^ result)) & 0x80u) != 0) {
        flags |= kFlagPV;
    }
    if ((full & 0x0100u) != 0) {
        flags |= kFlagC;
    }
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_add16_hl(ColecoCpuState* state, uint16_t value)
{
    const uint16_t lhs = state->hl;
    const uint32_t full = static_cast<uint32_t>(lhs) + static_cast<uint32_t>(value);
    const uint16_t result = static_cast<uint16_t>(full & 0xFFFFu);
    uint8_t flags = static_cast<uint8_t>(coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV));
    flags |= static_cast<uint8_t>((result >> 8) & (kFlagX | kFlagY));
    if (((lhs & 0x0FFFu) + (value & 0x0FFFu)) > 0x0FFFu) {
        flags |= kFlagH;
    }
    if ((full & 0x10000u) != 0) {
        flags |= kFlagC;
    }
    state->hl = result;
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_adc16_hl(ColecoCpuState* state, uint16_t value)
{
    const uint32_t lhs = state->hl;
    const uint32_t carry = (coleco_cpu_f(state) & kFlagC) ? 1u : 0u;
    const uint32_t full = lhs + static_cast<uint32_t>(value) + carry;
    const uint16_t result = static_cast<uint16_t>(full & 0xFFFFu);
    uint8_t flags = static_cast<uint8_t>((result >> 8) & (kFlagS | kFlagX | kFlagY));
    if (result == 0) {
        flags |= kFlagZ;
    }
    if (((lhs & 0x0FFFu) + (static_cast<uint32_t>(value) & 0x0FFFu) + carry) > 0x0FFFu) {
        flags |= kFlagH;
    }
    if (((~(lhs ^ static_cast<uint32_t>(value)) & (lhs ^ result)) & 0x8000u) != 0) {
        flags |= kFlagPV;
    }
    if ((full & 0x10000u) != 0) {
        flags |= kFlagC;
    }
    state->hl = result;
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_sbc16_hl(ColecoCpuState* state, uint16_t value)
{
    const uint32_t lhs = state->hl;
    const uint32_t carry = (coleco_cpu_f(state) & kFlagC) ? 1u : 0u;
    const uint32_t full = lhs - static_cast<uint32_t>(value) - carry;
    const uint16_t result = static_cast<uint16_t>(full & 0xFFFFu);
    uint8_t flags = static_cast<uint8_t>(((result >> 8) & (kFlagS | kFlagX | kFlagY)) | kFlagN);
    if (result == 0) {
        flags |= kFlagZ;
    }
    if ((static_cast<uint32_t>(lhs & 0x0FFFu) - static_cast<uint32_t>(value & 0x0FFFu) - carry) & 0x1000u) {
        flags |= kFlagH;
    }
    if ((((lhs ^ static_cast<uint32_t>(value)) & (lhs ^ result)) & 0x8000u) != 0) {
        flags |= kFlagPV;
    }
    if ((full & 0x10000u) != 0) {
        flags |= kFlagC;
    }
    state->hl = result;
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_daa(ColecoCpuState* state)
{
    const uint8_t oldA = coleco_cpu_a(state);
    uint8_t adjust = 0;
    uint8_t flags = coleco_cpu_f(state);
    bool carry = (flags & kFlagC) != 0;

    if ((flags & kFlagN) == 0) {
        if ((flags & kFlagH) != 0 || (oldA & 0x0Fu) > 0x09u) {
            adjust |= 0x06u;
        }
        if (carry || oldA > 0x99u) {
            adjust |= 0x60u;
            carry = true;
        }
        coleco_cpu_set_a(state, static_cast<uint8_t>(oldA + adjust));
    } else {
        if ((flags & kFlagH) != 0) {
            adjust |= 0x06u;
        }
        if (carry) {
            adjust |= 0x60u;
        }
        coleco_cpu_set_a(state, static_cast<uint8_t>(oldA - adjust));
    }

    const uint8_t result = coleco_cpu_a(state);
    uint8_t newFlags = static_cast<uint8_t>(flags & kFlagN);
    newFlags |= coleco_flags_szpxy(result);
    if ((((oldA ^ result) ^ adjust) & 0x10u) != 0) {
        newFlags |= kFlagH;
    }
    if (carry) {
        newFlags |= kFlagC;
    }
    coleco_cpu_set_f(state, newFlags);
}

void coleco_cpu_acc_rotate_left_carry(ColecoCpuState* state)
{
    const uint8_t value = coleco_cpu_a(state);
    const uint8_t result = static_cast<uint8_t>((value << 1) | (value >> 7));
    uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x80u) {
        flags |= kFlagC;
    }
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_acc_rotate_right_carry(ColecoCpuState* state)
{
    const uint8_t value = coleco_cpu_a(state);
    const uint8_t result = static_cast<uint8_t>((value >> 1) | (value << 7));
    uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x01u) {
        flags |= kFlagC;
    }
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_acc_rotate_left(ColecoCpuState* state)
{
    const uint8_t value = coleco_cpu_a(state);
    const uint8_t carry = (coleco_cpu_f(state) & kFlagC) ? 1u : 0u;
    const uint8_t result = static_cast<uint8_t>((value << 1) | carry);
    uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x80u) {
        flags |= kFlagC;
    }
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_acc_rotate_right(ColecoCpuState* state)
{
    const uint8_t value = coleco_cpu_a(state);
    const uint8_t carry = (coleco_cpu_f(state) & kFlagC) ? 0x80u : 0u;
    const uint8_t result = static_cast<uint8_t>((value >> 1) | carry);
    uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x01u) {
        flags |= kFlagC;
    }
    coleco_cpu_set_a(state, result);
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_set_in_flags(ColecoCpuState* state, uint8_t value)
{
    coleco_cpu_set_f(state, static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | coleco_flags_szpxy(value)));
}

void coleco_cpu_mark_unsupported(ColecoCpuState* state, uint8_t opcode)
{
    state->unsupportedOpcode = opcode;
    state->unsupportedPc = state->lastPc;
    state->runState = ColecoCpuRunState::Unsupported;
}

int coleco_cpu_service_irq(ColecoCpuState* state, ColecoMemoryState* memory)
{
    if (!state || !memory) {
        return 0;
    }

    const bool wasHalted = state->halted;
    state->irqPending = false;
    if (!state->iff1) {
        return 0;
    }

    state->iff1 = false;
    state->iff2 = false;
    state->halted = false;
    state->runState = ColecoCpuRunState::Running;
    #if COLECO_CPU_TRACE_ENABLED
    static uint16_t s_haltWakeLogCount = 0u;
    if (wasHalted && s_haltWakeLogCount < 8u) {
        const uint8_t resume0 = coleco_memory_read8(memory, state->pc);
        const uint8_t resume1 = coleco_memory_read8(memory, static_cast<uint16_t>(state->pc + 1u));
        const uint8_t resume2 = coleco_memory_read8(memory, static_cast<uint16_t>(state->pc + 2u));
        const uint8_t hook0 = coleco_memory_read8(memory, 0xFD9Au);
        const uint8_t hook1 = coleco_memory_read8(memory, 0xFD9Bu);
        const uint8_t hook2 = coleco_memory_read8(memory, 0xFD9Cu);
        std::printf("[MSX][HALT] wake at=%04X resume=%04X im=%u A8=%02X SSL3=%02X cart=%s banks=%u/%u/%u/%u bytes=%02X %02X %02X hook=%02X %02X %02X #%u\n",
                    static_cast<unsigned>(state->lastPc),
                    static_cast<unsigned>(state->pc),
                    static_cast<unsigned>(state->im),
                    0,
                    0,
                    "ROM",
                    0,
                    0,
                    0,
                    0,
                    static_cast<unsigned>(resume0),
                    static_cast<unsigned>(resume1),
                    static_cast<unsigned>(resume2),
                    static_cast<unsigned>(hook0),
                    static_cast<unsigned>(hook1),
                    static_cast<unsigned>(hook2),
                    static_cast<unsigned>(s_haltWakeLogCount));
        ++s_haltWakeLogCount;
    }
    #endif
    coleco_cpu_push16(state, memory, state->pc);

    if (state->im == 2u) {
        // IM 2: vector table at (I << 8) | 0xFF
        const uint16_t vecAddr = static_cast<uint16_t>((static_cast<uint16_t>(state->i) << 8) | 0xFFu);
        state->pc = coleco_cpu_mem_read16(memory, vecAddr);
        if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc) || coleco_cpu_trace_addr(state->sp)) {
            coleco_cpu_log_flow("IRQ2", state, memory, state->lastPc, state->pc);
        }
        return 19;
    }

    // IM 0 / IM 1: jump to 0x0038
    state->pc = 0x0038u;
    if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc) || coleco_cpu_trace_addr(state->sp)) {
        coleco_cpu_log_flow("IRQ", state, memory, state->lastPc, state->pc);
    }
    return 13;
}

int coleco_cpu_service_nmi(ColecoCpuState* state, ColecoMemoryState* memory)
{
    if (!state || !memory) {
        return 0;
    }

    state->nmiPending = false;
    state->halted = false;
    state->runState = ColecoCpuRunState::Running;
    
    coleco_cpu_push16(state, memory, state->pc);
    state->iff1 = false;
    state->pc = 0x0066u;
    if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc) || coleco_cpu_trace_addr(state->sp)) {
        coleco_cpu_log_flow("NMI", state, memory, state->lastPc, state->pc);
    }
    return 11;
}

void coleco_cpu_do_alu(ColecoCpuState* state, uint8_t aluOp, uint8_t value)
{
    switch (aluOp & 0x07u) {
        case 0: coleco_cpu_add8(state, coleco_cpu_a(state), value, 0); break;
        case 1: coleco_cpu_add8(state, coleco_cpu_a(state), value, (coleco_cpu_f(state) & kFlagC) ? 1u : 0u); break;
        case 2: coleco_cpu_sub8(state, coleco_cpu_a(state), value, 0); break;
        case 3: coleco_cpu_sub8(state, coleco_cpu_a(state), value, (coleco_cpu_f(state) & kFlagC) ? 1u : 0u); break;
        case 4: coleco_cpu_logic_and(state, value); break;
        case 5: coleco_cpu_logic_xor(state, value); break;
        case 6: coleco_cpu_logic_or(state, value); break;
        default: coleco_cpu_compare8(state, value); break;
    }
}

uint8_t coleco_cpu_cb_rotate(ColecoCpuState* state, uint8_t operation, uint8_t value)
{
    uint8_t result = value;
    uint8_t flags = 0;

    switch (operation & 0x07u) {
        case 0:
            result = static_cast<uint8_t>((value << 1) | (value >> 7));
            if (value & 0x80u) flags |= kFlagC;
            break;
        case 1:
            result = static_cast<uint8_t>((value >> 1) | (value << 7));
            if (value & 0x01u) flags |= kFlagC;
            break;
        case 2: {
            const uint8_t carry = (coleco_cpu_f(state) & kFlagC) ? 1u : 0u;
            result = static_cast<uint8_t>((value << 1) | carry);
            if (value & 0x80u) flags |= kFlagC;
            break;
        }
        case 3: {
            const uint8_t carry = (coleco_cpu_f(state) & kFlagC) ? 0x80u : 0u;
            result = static_cast<uint8_t>((value >> 1) | carry);
            if (value & 0x01u) flags |= kFlagC;
            break;
        }
        case 4:
            result = static_cast<uint8_t>(value << 1);
            if (value & 0x80u) flags |= kFlagC;
            break;
        case 5:
            result = static_cast<uint8_t>((value >> 1) | (value & 0x80u));
            if (value & 0x01u) flags |= kFlagC;
            break;
        case 6:
            // SLL / SL1 (undocumented): shift left, force bit 0 = 1, carry ← old bit 7.
            result = static_cast<uint8_t>((value << 1) | 0x01u);
            if (value & 0x80u) flags |= kFlagC;
            break;
        default:
            result = static_cast<uint8_t>(value >> 1);
            if (value & 0x01u) flags |= kFlagC;
            break;
    }

    flags |= coleco_flags_szpxy(result);
    coleco_cpu_set_f(state, flags);
    return result;
}

// ---- IX / IY prefix helpers (DD / FD) ----
// Based on fMSX Z80 engine by Marat Fayzullin, as ported to ESP32 in esplay-fMSX.
// Adapted to the existing code style: explicit switch dispatch instead of the
// canonical CodesXX.h macro trick, same correctness guarantees.

inline uint8_t coleco_cpu_xyh(const uint16_t* xy)
{
    return static_cast<uint8_t>(*xy >> 8);
}

inline uint8_t coleco_cpu_xyl(const uint16_t* xy)
{
    return static_cast<uint8_t>(*xy & 0xFFu);
}

inline void coleco_cpu_set_xyh(uint16_t* xy, uint8_t v)
{
    *xy = static_cast<uint16_t>((*xy & 0x00FFu) | (static_cast<uint16_t>(v) << 8));
}

inline void coleco_cpu_set_xyl(uint16_t* xy, uint8_t v)
{
    *xy = static_cast<uint16_t>((*xy & 0xFF00u) | v);
}

// Consume displacement byte and return effective address (IX+d) or (IY+d).
inline uint16_t coleco_cpu_xy_ea(ColecoCpuState* state, const ColecoMemoryState* memory, const uint16_t* xy)
{
    const int8_t disp = static_cast<int8_t>(coleco_cpu_fetch8(state, memory));
    return static_cast<uint16_t>(static_cast<int32_t>(*xy) + static_cast<int32_t>(disp));
}

// ADD IX,rr / ADD IY,rr — same as coleco_cpu_add16_hl but uses xy in place of HL.
void coleco_cpu_add16_xy(ColecoCpuState* state, uint16_t* xy, uint16_t value)
{
    const uint16_t lhs = *xy;
    const uint32_t full = static_cast<uint32_t>(lhs) + static_cast<uint32_t>(value);
    const uint16_t result = static_cast<uint16_t>(full & 0xFFFFu);
    uint8_t flags = static_cast<uint8_t>(coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV));
    flags |= static_cast<uint8_t>((result >> 8) & (kFlagX | kFlagY));
    if (((lhs & 0x0FFFu) + (value & 0x0FFFu)) > 0x0FFFu) {
        flags |= kFlagH;
    }
    if ((full & 0x10000u) != 0u) {
        flags |= kFlagC;
    }
    *xy = result;
    coleco_cpu_set_f(state, flags);
}

// DDCB / FDCB: indexed bit operations.
// Encoding: DD CB <disp> <op> — displacement is fetched before the operation opcode.
int coleco_cpu_step_xycb(ColecoCpuState* state, ColecoMemoryState* memory, const uint16_t* xy)
{
    const int8_t  disp   = static_cast<int8_t>(coleco_cpu_fetch8(state, memory));
    const uint16_t ea    = static_cast<uint16_t>(static_cast<int32_t>(*xy) + static_cast<int32_t>(disp));
    const uint8_t  opcode = coleco_cpu_fetch8(state, memory);
    const uint8_t  group  = static_cast<uint8_t>(opcode >> 6);
    const uint8_t  y      = static_cast<uint8_t>((opcode >> 3) & 0x07u);
    const uint8_t  z      = static_cast<uint8_t>(opcode & 0x07u);
    const uint8_t  value  = coleco_cpu_mem_read8(memory, ea);

    if (group == 0u) {
        // Rotate / shift on (IX+d)
        const uint8_t result = coleco_cpu_cb_rotate(state, y, value);
        coleco_cpu_mem_write8(memory, ea, result);
        if (z != 6u) {
            coleco_cpu_set_reg8(state, memory, z, result);
        }
        return 23;
    }

    if (group == 1u) {
        // BIT y,(IX+d)
        uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | kFlagH);
        if ((value & static_cast<uint8_t>(1u << y)) == 0u) {
            flags |= static_cast<uint8_t>(kFlagZ | kFlagPV);
        }
        if ((y == 7u) && ((value & 0x80u) != 0u)) {
            flags |= kFlagS;
        }
        coleco_cpu_set_f(state, flags);
        return 20;
    }

    // RES y,(IX+d) or SET y,(IX+d)
    const uint8_t result = (group == 2u)
        ? static_cast<uint8_t>(value & ~static_cast<uint8_t>(1u << y))
        : static_cast<uint8_t>(value |  static_cast<uint8_t>(1u << y));
    coleco_cpu_mem_write8(memory, ea, result);
    if (z != 6u) {
        coleco_cpu_set_reg8(state, memory, z, result);
    }
    return 23;
}

// DD / FD prefix handler.
// xy points to state->ix (DD) or state->iy (FD).
// For opcodes that have no IX/IY variant the prefix is silently discarded and
// the opcode is re-decoded by the base dispatcher — correct Z80 behaviour.
int coleco_cpu_step_xy(ColecoCpuState* state, ColecoMemoryState* memory, uint16_t* xy)
{
    const uint8_t opcode = coleco_cpu_fetch8(state, memory);

    switch (opcode) {
        // 16-bit loads and arithmetic — HL replaced by IX/IY
        case 0x09: coleco_cpu_add16_xy(state, xy, state->bc); return 15;
        case 0x19: coleco_cpu_add16_xy(state, xy, state->de); return 15;
        case 0x21: *xy = coleco_cpu_fetch16(state, memory); return 14;
        case 0x22: { const uint16_t a = coleco_cpu_fetch16(state, memory); coleco_cpu_mem_write16(memory, a, *xy); return 20; }
        case 0x23: *xy = static_cast<uint16_t>(*xy + 1u); return 10;
        case 0x29: coleco_cpu_add16_xy(state, xy, *xy); return 15;
        case 0x2A: { const uint16_t a = coleco_cpu_fetch16(state, memory); *xy = coleco_cpu_mem_read16(memory, a); return 20; }
        case 0x2B: *xy = static_cast<uint16_t>(*xy - 1u); return 10;
        case 0x39: coleco_cpu_add16_xy(state, xy, state->sp); return 15;

        // IXH / IXL operations (documented on real silicon; used by MSX BIOS)
        case 0x24: coleco_cpu_set_xyh(xy, coleco_cpu_inc8(state, coleco_cpu_xyh(xy))); return 4;
        case 0x25: coleco_cpu_set_xyh(xy, coleco_cpu_dec8(state, coleco_cpu_xyh(xy))); return 4;
        case 0x26: coleco_cpu_set_xyh(xy, coleco_cpu_fetch8(state, memory)); return 7;
        case 0x2C: coleco_cpu_set_xyl(xy, coleco_cpu_inc8(state, coleco_cpu_xyl(xy))); return 4;
        case 0x2D: coleco_cpu_set_xyl(xy, coleco_cpu_dec8(state, coleco_cpu_xyl(xy))); return 4;
        case 0x2E: coleco_cpu_set_xyl(xy, coleco_cpu_fetch8(state, memory)); return 7;

        // LD r, IXH/IXL
        case 0x44: coleco_set_hi(&state->bc, coleco_cpu_xyh(xy)); return 4;
        case 0x45: coleco_set_hi(&state->bc, coleco_cpu_xyl(xy)); return 4;
        case 0x4C: coleco_set_lo(&state->bc, coleco_cpu_xyh(xy)); return 4;
        case 0x4D: coleco_set_lo(&state->bc, coleco_cpu_xyl(xy)); return 4;
        case 0x54: coleco_set_hi(&state->de, coleco_cpu_xyh(xy)); return 4;
        case 0x55: coleco_set_hi(&state->de, coleco_cpu_xyl(xy)); return 4;
        case 0x5C: coleco_set_lo(&state->de, coleco_cpu_xyh(xy)); return 4;
        case 0x5D: coleco_set_lo(&state->de, coleco_cpu_xyl(xy)); return 4;
        case 0x7C: coleco_cpu_set_a(state, coleco_cpu_xyh(xy)); return 4;
        case 0x7D: coleco_cpu_set_a(state, coleco_cpu_xyl(xy)); return 4;

        // LD IXH, r
        case 0x60: coleco_cpu_set_xyh(xy, coleco_hi(state->bc));  return 4;
        case 0x61: coleco_cpu_set_xyh(xy, coleco_lo(state->bc));  return 4;
        case 0x62: coleco_cpu_set_xyh(xy, coleco_hi(state->de));  return 4;
        case 0x63: coleco_cpu_set_xyh(xy, coleco_lo(state->de));  return 4;
        case 0x64: /* LD IXH,IXH */ return 4;
        case 0x65: coleco_cpu_set_xyh(xy, coleco_cpu_xyl(xy));    return 4;
        case 0x67: coleco_cpu_set_xyh(xy, coleco_cpu_a(state));   return 4;

        // LD IXL, r
        case 0x68: coleco_cpu_set_xyl(xy, coleco_hi(state->bc));  return 4;
        case 0x69: coleco_cpu_set_xyl(xy, coleco_lo(state->bc));  return 4;
        case 0x6A: coleco_cpu_set_xyl(xy, coleco_hi(state->de));  return 4;
        case 0x6B: coleco_cpu_set_xyl(xy, coleco_lo(state->de));  return 4;
        case 0x6C: coleco_cpu_set_xyl(xy, coleco_cpu_xyh(xy));    return 4;
        case 0x6D: /* LD IXL,IXL */ return 4;
        case 0x6F: coleco_cpu_set_xyl(xy, coleco_cpu_a(state));   return 4;

        // ALU A, IXH
        case 0x84: coleco_cpu_do_alu(state, 0, coleco_cpu_xyh(xy)); return 4;
        case 0x8C: coleco_cpu_do_alu(state, 1, coleco_cpu_xyh(xy)); return 4;
        case 0x94: coleco_cpu_do_alu(state, 2, coleco_cpu_xyh(xy)); return 4;
        case 0x9C: coleco_cpu_do_alu(state, 3, coleco_cpu_xyh(xy)); return 4;
        case 0xA4: coleco_cpu_do_alu(state, 4, coleco_cpu_xyh(xy)); return 4;
        case 0xAC: coleco_cpu_do_alu(state, 5, coleco_cpu_xyh(xy)); return 4;
        case 0xB4: coleco_cpu_do_alu(state, 6, coleco_cpu_xyh(xy)); return 4;
        case 0xBC: coleco_cpu_do_alu(state, 7, coleco_cpu_xyh(xy)); return 4;

        // ALU A, IXL
        case 0x85: coleco_cpu_do_alu(state, 0, coleco_cpu_xyl(xy)); return 4;
        case 0x8D: coleco_cpu_do_alu(state, 1, coleco_cpu_xyl(xy)); return 4;
        case 0x95: coleco_cpu_do_alu(state, 2, coleco_cpu_xyl(xy)); return 4;
        case 0x9D: coleco_cpu_do_alu(state, 3, coleco_cpu_xyl(xy)); return 4;
        case 0xA5: coleco_cpu_do_alu(state, 4, coleco_cpu_xyl(xy)); return 4;
        case 0xAD: coleco_cpu_do_alu(state, 5, coleco_cpu_xyl(xy)); return 4;
        case 0xB5: coleco_cpu_do_alu(state, 6, coleco_cpu_xyl(xy)); return 4;
        case 0xBD: coleco_cpu_do_alu(state, 7, coleco_cpu_xyl(xy)); return 4;

        // INC / DEC / LD on indexed memory (IX+d)
        case 0x34: {
            const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy);
            coleco_cpu_mem_write8(memory, ea, coleco_cpu_inc8(state, coleco_cpu_mem_read8(memory, ea)));
            return 23;
        }
        case 0x35: {
            const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy);
            coleco_cpu_mem_write8(memory, ea, coleco_cpu_dec8(state, coleco_cpu_mem_read8(memory, ea)));
            return 23;
        }
        case 0x36: {
            const uint16_t ea  = coleco_cpu_xy_ea(state, memory, xy);
            const uint8_t  imm = coleco_cpu_fetch8(state, memory);
            coleco_cpu_mem_write8(memory, ea, imm);
            return 19;
        }

        // LD r,(IX+d) — destination is always the true register (not IXH/IXL)
        case 0x46: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_set_hi(&state->bc, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x4E: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_set_lo(&state->bc, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x56: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_set_hi(&state->de, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x5E: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_set_lo(&state->de, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x66: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_set_hi(&state->hl, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x6E: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_set_lo(&state->hl, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x7E: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_set_a(state, coleco_cpu_mem_read8(memory, ea)); return 19; }

        // LD (IX+d),r — source is always the true register (not IXH/IXL)
        case 0x70: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_mem_write8(memory, ea, coleco_hi(state->bc)); return 19; }
        case 0x71: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_mem_write8(memory, ea, coleco_lo(state->bc)); return 19; }
        case 0x72: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_mem_write8(memory, ea, coleco_hi(state->de)); return 19; }
        case 0x73: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_mem_write8(memory, ea, coleco_lo(state->de)); return 19; }
        case 0x74: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_mem_write8(memory, ea, coleco_hi(state->hl)); return 19; }
        case 0x75: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_mem_write8(memory, ea, coleco_lo(state->hl)); return 19; }
        case 0x77: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_mem_write8(memory, ea, coleco_cpu_a(state)); return 19; }

        // ALU A,(IX+d)
        case 0x86: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 0, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x8E: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 1, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x96: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 2, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0x9E: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 3, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0xA6: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 4, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0xAE: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 5, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0xB6: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 6, coleco_cpu_mem_read8(memory, ea)); return 19; }
        case 0xBE: { const uint16_t ea = coleco_cpu_xy_ea(state, memory, xy); coleco_cpu_do_alu(state, 7, coleco_cpu_mem_read8(memory, ea)); return 19; }

        // Stack and jump with IX/IY
        case 0xE1: *xy = coleco_cpu_pop16(state, memory); return 14;
        case 0xE3: {
            const uint16_t top = coleco_cpu_mem_read16(memory, state->sp);
            coleco_cpu_mem_write16(memory, state->sp, *xy);
            *xy = top;
            return 23;
        }
        case 0xE5: coleco_cpu_push16(state, memory, *xy); return 15;
        case 0xE9:
            coleco_cpu_restore_cart_boot_mapping(memory, *xy, state->lastPc);
            state->pc = *xy;
            return 8;
        case 0xF9: state->sp = *xy; return 10;

        // DDCB / FDCB: 4-byte indexed bit operations
        case 0xCB: return coleco_cpu_step_xycb(state, memory, xy);

        // Nested DD/FD: second prefix restarts with new register target
        case 0xDD: return coleco_cpu_step_xy(state, memory, &state->ix);
        case 0xFD: return coleco_cpu_step_xy(state, memory, &state->iy);

        // Unrecognised opcode after DD/FD: prefix is ignored on real Z80.
        // Undo the fetch so the base dispatcher sees the opcode unmodified.
        default:
            state->pc = static_cast<uint16_t>(state->pc - 1u);
            state->r  = static_cast<uint8_t>(state->r  - 1u);
            return coleco_cpu_step_opcode(state, memory);
    }
}

int coleco_cpu_step_cb(ColecoCpuState* state, ColecoMemoryState* memory)
{
    const uint8_t opcode = coleco_cpu_fetch8(state, memory);
    const uint8_t group = static_cast<uint8_t>(opcode >> 6);
    const uint8_t y = static_cast<uint8_t>((opcode >> 3) & 0x07u);
    const uint8_t z = static_cast<uint8_t>(opcode & 0x07u);
    const uint8_t value = coleco_cpu_get_reg8(state, memory, z);

    if (group == 0) {
        const uint8_t result = coleco_cpu_cb_rotate(state, y, value);
        coleco_cpu_set_reg8(state, memory, z, result);
        return z == 6 ? 15 : 8;
    }

    if (group == 1) {
        uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | kFlagH);
        if ((value & static_cast<uint8_t>(1u << y)) == 0) {
            flags |= static_cast<uint8_t>(kFlagZ | kFlagPV);
        }
        // BIT only updates S when testing bit 7 and that bit is set.
        if ((y == 7u) && ((value & 0x80u) != 0u)) {
            flags |= kFlagS;
        }
        flags |= static_cast<uint8_t>(value & (kFlagX | kFlagY));
        coleco_cpu_set_f(state, flags);
        return z == 6 ? 12 : 8;
    }

    uint8_t result = value;
    if (group == 2) {
        result = static_cast<uint8_t>(value & ~static_cast<uint8_t>(1u << y));
    } else {
        result = static_cast<uint8_t>(value | static_cast<uint8_t>(1u << y));
    }

    coleco_cpu_set_reg8(state, memory, z, result);
    return z == 6 ? 15 : 8;
}

void coleco_cpu_block_ldi(ColecoCpuState* state, ColecoMemoryState* memory, int direction)
{
    const uint8_t value = coleco_cpu_mem_read8(memory, state->hl);
    coleco_cpu_mem_write8(memory, state->de, value);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    state->de = static_cast<uint16_t>(state->de + direction);
    state->bc = static_cast<uint16_t>(state->bc - 1u);

    uint8_t flags = static_cast<uint8_t>(coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagC));
    if (state->bc != 0) {
        flags |= kFlagPV;
    }
    const uint8_t mix = static_cast<uint8_t>(coleco_cpu_a(state) + value);
    flags |= static_cast<uint8_t>(mix & (kFlagX | kFlagY));
    coleco_cpu_set_f(state, flags);
}

void coleco_cpu_block_outi(ColecoCpuState* state, ColecoMemoryState* memory, int direction)
{
    const uint8_t value = coleco_cpu_mem_read8(memory, state->hl);
    coleco_memory_out(memory, coleco_lo(state->bc), value);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    coleco_set_hi(&state->bc, static_cast<uint8_t>(coleco_hi(state->bc) - 1u));

    uint8_t flags = static_cast<uint8_t>(kFlagN | (coleco_hi(state->bc) & (kFlagS | kFlagX | kFlagY)));
    if (coleco_hi(state->bc) == 0) {
        flags |= kFlagZ;
    } else {
        flags |= kFlagPV;
    }
    coleco_cpu_set_f(state, flags);
}

// CPI / CPD / CPIR / CPDR: compare A with (HL), HL±=1, BC-=1.
// PV flag reflects BC!=0 after decrement; Z reflects match (A == mem value).
void coleco_cpu_block_cpi(ColecoCpuState* state, ColecoMemoryState* memory, int direction)
{
    const uint8_t mem    = coleco_cpu_mem_read8(memory, state->hl);
    const uint8_t a      = coleco_cpu_a(state);
    const uint8_t result = static_cast<uint8_t>(a - mem);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    state->bc = static_cast<uint16_t>(state->bc - 1u);

    uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | kFlagN);
    flags |= static_cast<uint8_t>(result & kFlagS);
    if (result == 0u) {
        flags |= kFlagZ;
    }
    if ((a & 0x0Fu) < (mem & 0x0Fu)) {
        flags |= kFlagH;
    }
    if (state->bc != 0u) {
        flags |= kFlagPV;
    }
    // X/Y come from bits 3 and 5 of (result - H) on a real Z80.
    const uint8_t n = static_cast<uint8_t>(result - ((flags & kFlagH) ? 1u : 0u));
    flags |= static_cast<uint8_t>(n & (kFlagX | kFlagY));
    coleco_cpu_set_f(state, flags);
}

// INI / IND / INIR / INDR: read one byte from port (C) into memory[HL], HL±=1, B-=1.
void coleco_cpu_block_ini(ColecoCpuState* state, ColecoMemoryState* memory, int direction)
{
    const uint8_t value = coleco_memory_in(memory, coleco_lo(state->bc));
    coleco_cpu_mem_write8(memory, state->hl, value);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    coleco_set_hi(&state->bc, static_cast<uint8_t>(coleco_hi(state->bc) - 1u));

    uint8_t flags = static_cast<uint8_t>(kFlagN | (coleco_hi(state->bc) & (kFlagS | kFlagX | kFlagY)));
    if (coleco_hi(state->bc) == 0) {
        flags |= kFlagZ;
    } else {
        flags |= kFlagPV;
    }
    coleco_cpu_set_f(state, flags);
}

int coleco_cpu_step_ed(ColecoCpuState* state, ColecoMemoryState* memory)
{
    const uint8_t opcode = coleco_cpu_fetch8(state, memory);
    const uint8_t regPair = static_cast<uint8_t>((opcode >> 4) & 0x03u);

    switch (opcode) {
        case 0x40:
        case 0x48:
        case 0x50:
        case 0x58:
        case 0x60:
        case 0x68:
        case 0x78: {
            const uint8_t value = coleco_memory_in(memory, coleco_lo(state->bc));
            coleco_cpu_set_reg8(state, memory, static_cast<uint8_t>((opcode >> 3) & 0x07u), value);
            coleco_cpu_set_in_flags(state, value);
            return 12;
        }
        case 0x41:
        case 0x49:
        case 0x51:
        case 0x59:
        case 0x61:
        case 0x69:
        case 0x79: {
            const uint8_t value = coleco_cpu_get_reg8(state, memory, static_cast<uint8_t>((opcode >> 3) & 0x07u));
            coleco_memory_out(memory, coleco_lo(state->bc), value);
            return 12;
        }
        case 0x42:
        case 0x52:
        case 0x62:
        case 0x72:
            coleco_cpu_sbc16_hl(state, *coleco_cpu_reg16_ptr(state, regPair));
            return 15;
        case 0x4A:
        case 0x5A:
        case 0x6A:
        case 0x7A:
            coleco_cpu_adc16_hl(state, *coleco_cpu_reg16_ptr(state, regPair));
            return 15;
        case 0x43:
        case 0x53:
        case 0x63:
        case 0x73: {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            coleco_cpu_mem_write16(memory, address, *coleco_cpu_reg16_ptr(state, regPair));
            return 20;
        }
        case 0x4B:
        case 0x5B:
        case 0x6B:
        case 0x7B: {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            *coleco_cpu_reg16_ptr(state, regPair) = coleco_cpu_mem_read16(memory, address);
            return 20;
        }
        case 0x44:
        case 0x4C:
        case 0x54:
        case 0x5C:
        case 0x64:
        case 0x6C:
        case 0x74:
        case 0x7C:
            coleco_cpu_sub8(state, 0, coleco_cpu_a(state), 0);
            return 8;
        case 0x45:
        case 0x4D:
        case 0x55:
        case 0x5D:
        case 0x65:
        case 0x6D:
        case 0x75:
        case 0x7D: {
            const uint16_t from = state->lastPc;
            state->pc = coleco_cpu_pop16(state, memory);
            state->iff1 = state->iff2;
            if (coleco_cpu_trace_pc(from) || coleco_cpu_trace_pc(state->pc)) {
                coleco_cpu_log_flow("RETN", state, memory, from, state->pc);
            }
            return 14;
        }
        case 0x46:
        case 0x4E:
        case 0x66:
        case 0x6E:
            state->im = 0;
            return 8;
        case 0x56:
        case 0x76:
            state->im = 1;
            return 8;
        case 0x5E:
        case 0x7E:
            state->im = 2;
            return 8;
        case 0x47:
            state->i = coleco_cpu_a(state);
            return 9;
        case 0x4F:
            state->r = coleco_cpu_a(state);
            return 9;
        case 0x57: {
            const uint8_t value = state->i;
            coleco_cpu_set_a(state, value);
            uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | coleco_flags_szxy(value));
            if (state->iff2) {
                flags |= kFlagPV;
            }
            coleco_cpu_set_f(state, flags);
            return 9;
        }
        case 0x5F: {
            const uint8_t value = state->r;
            coleco_cpu_set_a(state, value);
            uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | coleco_flags_szxy(value));
            if (state->iff2) {
                flags |= kFlagPV;
            }
            coleco_cpu_set_f(state, flags);
            return 9;
        }
        case 0x70: {
            const uint8_t value = coleco_memory_in(memory, coleco_lo(state->bc));
            coleco_cpu_set_in_flags(state, value);
            return 12;
        }
        case 0x71:
            coleco_memory_out(memory, coleco_lo(state->bc), 0x00u);
            return 12;
        case 0x77:
        case 0x7F:
            return 8;
        case 0xA0:
            coleco_cpu_block_ldi(state, memory, 1);
            return 16;
        case 0xA8:
            coleco_cpu_block_ldi(state, memory, -1);
            return 16;
        case 0xB0:
            coleco_cpu_block_ldi(state, memory, 1);
            if (state->bc != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xB8:
            coleco_cpu_block_ldi(state, memory, -1);
            if (state->bc != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xA3:
            coleco_cpu_block_outi(state, memory, 1);
            return 16;
        case 0xAB:
            coleco_cpu_block_outi(state, memory, -1);
            return 16;
        case 0xB3:
            coleco_cpu_block_outi(state, memory, 1);
            if (coleco_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xBB:
            coleco_cpu_block_outi(state, memory, -1);
            if (coleco_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xA2:                                   // INI
            coleco_cpu_block_ini(state, memory, 1);
            return 16;
        case 0xAA:                                   // IND
            coleco_cpu_block_ini(state, memory, -1);
            return 16;
        case 0xB2:                                   // INIR
            coleco_cpu_block_ini(state, memory, 1);
            if (coleco_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xBA:                                   // INDR
            coleco_cpu_block_ini(state, memory, -1);
            if (coleco_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xA1:                                   // CPI
            coleco_cpu_block_cpi(state, memory, 1);
            return 16;
        case 0xA9:                                   // CPD
            coleco_cpu_block_cpi(state, memory, -1);
            return 16;
        case 0xB1:                                   // CPIR
            coleco_cpu_block_cpi(state, memory, 1);
            if ((coleco_cpu_f(state) & kFlagPV) && !(coleco_cpu_f(state) & kFlagZ)) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xB9:                                   // CPDR
            coleco_cpu_block_cpi(state, memory, -1);
            if ((coleco_cpu_f(state) & kFlagPV) && !(coleco_cpu_f(state) & kFlagZ)) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0x67: {                                 // RRD
            const uint8_t mem = coleco_cpu_mem_read8(memory, state->hl);
            const uint8_t a   = coleco_cpu_a(state);
            coleco_cpu_mem_write8(memory, state->hl, static_cast<uint8_t>((a << 4) | (mem >> 4)));
            coleco_cpu_set_a(state, static_cast<uint8_t>((a & 0xF0u) | (mem & 0x0Fu)));
            coleco_cpu_set_f(state, static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | coleco_flags_szpxy(coleco_cpu_a(state))));
            return 18;
        }
        case 0x6F: {                                 // RLD
            const uint8_t mem = coleco_cpu_mem_read8(memory, state->hl);
            const uint8_t a   = coleco_cpu_a(state);
            coleco_cpu_mem_write8(memory, state->hl, static_cast<uint8_t>((mem << 4) | (a & 0x0Fu)));
            coleco_cpu_set_a(state, static_cast<uint8_t>((a & 0xF0u) | (mem >> 4)));
            coleco_cpu_set_f(state, static_cast<uint8_t>((coleco_cpu_f(state) & kFlagC) | coleco_flags_szpxy(coleco_cpu_a(state))));
            return 18;
        }
        case 0xFE:
            // BIOS software trap: ED FE is written by coleco_disk_apply_rom_patches()
            // at the DISK ROM entry points (PHYDIO, DSKCHG, GETDPB, DSKFMT, DRVOFF).
            
            return 8;
        default:
            // Unassigned ED-prefixed opcodes behave as NOPs on the Z80.
            return 8;
    }
}

int coleco_cpu_step_opcode(ColecoCpuState* state, ColecoMemoryState* memory)
{
    if (!state || !memory) {
        return 0;
    }

    state->lastPc = state->pc;
    coleco_cpu_log_context(state, memory, state->lastPc);
    const uint8_t opcode = coleco_cpu_fetch8(state, memory);
    coleco_cpu_log_opcode(state, memory, state->lastPc, opcode);

    state->lastOpcode = opcode;

    if ((opcode & 0xC7u) == 0x04u) {
        const uint8_t reg = static_cast<uint8_t>((opcode >> 3) & 0x07u);
        const uint8_t value = coleco_cpu_get_reg8(state, memory, reg);
        coleco_cpu_set_reg8(state, memory, reg, coleco_cpu_inc8(state, value));
        return reg == 6 ? 11 : 4;
    }

    if ((opcode & 0xC7u) == 0x05u) {
        const uint8_t reg = static_cast<uint8_t>((opcode >> 3) & 0x07u);
        const uint8_t value = coleco_cpu_get_reg8(state, memory, reg);
        coleco_cpu_set_reg8(state, memory, reg, coleco_cpu_dec8(state, value));
        return reg == 6 ? 11 : 4;
    }

    if ((opcode & 0xC7u) == 0x06u) {
        const uint8_t reg = static_cast<uint8_t>((opcode >> 3) & 0x07u);
        const uint8_t value = coleco_cpu_fetch8(state, memory);
        coleco_cpu_set_reg8(state, memory, reg, value);
        return reg == 6 ? 10 : 7;
    }

    if ((opcode & 0xCFu) == 0x01u) {
        *coleco_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)) = coleco_cpu_fetch16(state, memory);
        return 10;
    }

    if ((opcode & 0xCFu) == 0x03u) {
        uint16_t* reg = coleco_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u));
        *reg = static_cast<uint16_t>(*reg + 1u);
        return 6;
    }

    if ((opcode & 0xCFu) == 0x0Bu) {
        uint16_t* reg = coleco_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u));
        *reg = static_cast<uint16_t>(*reg - 1u);
        return 6;
    }

    if ((opcode & 0xCFu) == 0x09u) {
        coleco_cpu_add16_hl(state, *coleco_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)));
        return 11;
    }

    if ((opcode & 0xC0u) == 0x40u) {
        if (opcode == 0x76u) {
            state->halted = true;
            state->runState = ColecoCpuRunState::Halted;
            #if COLECO_CPU_TRACE_ENABLED
            static uint16_t s_haltEnterLogCount = 0u;
            if (s_haltEnterLogCount < 8u) {
                const uint8_t next0 = coleco_memory_read8(memory, state->lastPc);
                const uint8_t next1 = coleco_memory_read8(memory, static_cast<uint16_t>(state->lastPc + 1u));
                const uint8_t next2 = coleco_memory_read8(memory, static_cast<uint16_t>(state->lastPc + 2u));
                const uint8_t next3 = coleco_memory_read8(memory, static_cast<uint16_t>(state->lastPc + 3u));
                const uint8_t hook0 = coleco_memory_read8(memory, 0xFD9Au);
                const uint8_t hook1 = coleco_memory_read8(memory, 0xFD9Bu);
                const uint8_t hook2 = coleco_memory_read8(memory, 0xFD9Cu);
                std::printf("[MSX][HALT] enter at=%04X next=%04X iff1=%u irq=%u A8=%02X SSL3=%02X cart=%s banks=%u/%u/%u/%u bytes=%02X %02X %02X %02X hook=%02X %02X %02X #%u\n",
                            static_cast<unsigned>(state->lastPc),
                            static_cast<unsigned>(state->pc),
                            state->iff1 ? 1u : 0u,
                            state->irqPending ? 1u : 0u,
                            0,
                            0,
                            "ROM",
                            0,
                            0,
                            0,
                            0,
                            static_cast<unsigned>(next0),
                            static_cast<unsigned>(next1),
                            static_cast<unsigned>(next2),
                            static_cast<unsigned>(next3),
                            static_cast<unsigned>(hook0),
                            static_cast<unsigned>(hook1),
                            static_cast<unsigned>(hook2),
                            static_cast<unsigned>(s_haltEnterLogCount));
                ++s_haltEnterLogCount;
            }
            #endif
            return 4;
        }
        const uint8_t dst = static_cast<uint8_t>((opcode >> 3) & 0x07u);
        const uint8_t src = static_cast<uint8_t>(opcode & 0x07u);
        const uint8_t value = coleco_cpu_get_reg8(state, memory, src);
        coleco_cpu_set_reg8(state, memory, dst, value);
        return (dst == 6 || src == 6) ? 7 : 4;
    }

    if ((opcode & 0xC0u) == 0x80u) {
        const uint8_t value = coleco_cpu_get_reg8(state, memory, static_cast<uint8_t>(opcode & 0x07u));
        coleco_cpu_do_alu(state, static_cast<uint8_t>((opcode >> 3) & 0x07u), value);
        return (opcode & 0x07u) == 6 ? 7 : 4;
    }

    if ((opcode & 0xC7u) == 0xC0u) {
        if (coleco_cpu_condition(state, static_cast<uint8_t>((opcode >> 3) & 0x07u))) {
            const uint16_t from = state->lastPc;
            state->pc = coleco_cpu_pop16(state, memory);
            if (coleco_cpu_trace_pc(from) || coleco_cpu_trace_pc(state->pc)) {
                coleco_cpu_log_flow("RETcc", state, memory, from, state->pc);
            }
            return 11;
        }
        return 5;
    }

    if ((opcode & 0xC7u) == 0xC2u) {
        const uint16_t address = coleco_cpu_fetch16(state, memory);
        if (coleco_cpu_condition(state, static_cast<uint8_t>((opcode >> 3) & 0x07u))) {
            coleco_cpu_restore_cart_boot_mapping(memory, address, state->lastPc);
            if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(address)) {
                coleco_cpu_log_flow("JPcc", state, memory, state->lastPc, address);
            }
            state->pc = address;
        }
        return 10;
    }

    if ((opcode & 0xC7u) == 0xC4u) {
        const uint16_t address = coleco_cpu_fetch16(state, memory);
        if (coleco_cpu_condition(state, static_cast<uint8_t>((opcode >> 3) & 0x07u))) {
            coleco_cpu_push16(state, memory, state->pc);
            if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(address)) {
                coleco_cpu_log_flow("CALLcc", state, memory, state->lastPc, address);
            }
            state->pc = address;
            return 17;
        }
        return 10;
    }

    if ((opcode & 0xCFu) == 0xC1u) {
        *coleco_cpu_stack_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)) = coleco_cpu_pop16(state, memory);
        return 10;
    }

    if ((opcode & 0xCFu) == 0xC5u) {
        coleco_cpu_push16(state, memory, *coleco_cpu_stack_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)));
        return 11;
    }

    if ((opcode & 0xC7u) == 0xC7u) {
        coleco_cpu_push16(state, memory, state->pc);
        state->pc = static_cast<uint16_t>(opcode & 0x38u);
        if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc)) {
            coleco_cpu_log_flow("RST", state, memory, state->lastPc, state->pc);
        }
        return 11;
    }

    switch (opcode) {
        case 0x00:
            return 4;
        case 0x02:
            coleco_cpu_mem_write8(memory, state->bc, coleco_cpu_a(state));
            return 7;
        case 0x07:
            coleco_cpu_acc_rotate_left_carry(state);
            return 4;
        case 0x08:
            coleco_cpu_exchange16(&state->af, &state->af2);
            return 4;
        case 0x0A:
            coleco_cpu_set_a(state, coleco_cpu_mem_read8(memory, state->bc));
            return 7;
        case 0x0F:
            coleco_cpu_acc_rotate_right_carry(state);
            return 4;
        case 0x10: {
            const int8_t offset = coleco_signed_offset(coleco_cpu_fetch8(state, memory));
            coleco_set_hi(&state->bc, static_cast<uint8_t>(coleco_hi(state->bc) - 1u));
            if (coleco_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc + offset);
                if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc)) {
                    coleco_cpu_log_flow("DJNZ", state, memory, state->lastPc, state->pc);
                }
                return 13;
            }
            return 8;
        }
        case 0x12:
            coleco_cpu_mem_write8(memory, state->de, coleco_cpu_a(state));
            return 7;
        case 0x17:
            coleco_cpu_acc_rotate_left(state);
            return 4;
        case 0x18: {
            const int8_t offset = coleco_signed_offset(coleco_cpu_fetch8(state, memory));
            state->pc = static_cast<uint16_t>(state->pc + offset);
            if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc)) {
                coleco_cpu_log_flow("JR", state, memory, state->lastPc, state->pc);
            }
            return 12;
        }
        case 0x1A:
            coleco_cpu_set_a(state, coleco_cpu_mem_read8(memory, state->de));
            return 7;
        case 0x1F:
            coleco_cpu_acc_rotate_right(state);
            return 4;
        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38: {
            const int8_t offset = coleco_signed_offset(coleco_cpu_fetch8(state, memory));
            const uint8_t condition = static_cast<uint8_t>((opcode >> 3) & 0x03u);
            if (coleco_cpu_condition(state, condition)) {
                state->pc = static_cast<uint16_t>(state->pc + offset);
                if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc)) {
                    coleco_cpu_log_flow("JRcc", state, memory, state->lastPc, state->pc);
                }
                return 12;
            }
            return 7;
        }
        case 0x22: {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            coleco_cpu_mem_write16(memory, address, state->hl);
            return 16;
        }
        case 0x27:
            coleco_cpu_daa(state);
            return 4;
        case 0x2A: {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            state->hl = coleco_cpu_mem_read16(memory, address);
            return 16;
        }
        case 0x2F:
            coleco_cpu_set_a(state, static_cast<uint8_t>(coleco_cpu_a(state) ^ 0xFFu));
            coleco_cpu_set_f(state, static_cast<uint8_t>((coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV | kFlagC)) |
                                                      (coleco_cpu_a(state) & (kFlagX | kFlagY)) |
                                                      kFlagH | kFlagN));
            return 4;
        case 0x32: {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            coleco_cpu_mem_write8(memory, address, coleco_cpu_a(state));
            return 13;
        }
        case 0x37:
            coleco_cpu_set_f(state, static_cast<uint8_t>((coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) |
                                                      (coleco_cpu_a(state) & (kFlagX | kFlagY)) |
                                                      kFlagC));
            return 4;
        case 0x3A: {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            coleco_cpu_set_a(state, coleco_cpu_mem_read8(memory, address));
            return 13;
        }
        case 0x3F: {
            const bool oldCarry = (coleco_cpu_f(state) & kFlagC) != 0;
            uint8_t flags = static_cast<uint8_t>((coleco_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) |
                                                 (coleco_cpu_a(state) & (kFlagX | kFlagY)));
            if (oldCarry) {
                flags |= kFlagH;
            } else {
                flags |= kFlagC;
            }
            coleco_cpu_set_f(state, flags);
            return 4;
        }
        case 0xC3:
        {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            coleco_cpu_restore_cart_boot_mapping(memory, address, state->lastPc);
            state->pc = address;
            if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc)) {
                coleco_cpu_log_flow("JP", state, memory, state->lastPc, state->pc);
            }
            return 10;
        }
        case 0xC6:
            coleco_cpu_do_alu(state, 0, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xC9:
            state->pc = coleco_cpu_pop16(state, memory);
            if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->pc)) {
                coleco_cpu_log_flow("RET", state, memory, state->lastPc, state->pc);
            }
            return 10;
        case 0xCB:
            return coleco_cpu_step_cb(state, memory);
        case 0xCD: {
            const uint16_t address = coleco_cpu_fetch16(state, memory);
            coleco_cpu_push16(state, memory, state->pc);
            if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(address)) {
                coleco_cpu_log_flow("CALL", state, memory, state->lastPc, address);
            }
            state->pc = address;
            return 17;
        }
        case 0xCE:
            coleco_cpu_do_alu(state, 1, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xD3:
            coleco_memory_out(memory, coleco_cpu_fetch8(state, memory), coleco_cpu_a(state));
            return 11;
        case 0xD6:
            coleco_cpu_do_alu(state, 2, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xD9:
            coleco_cpu_exchange16(&state->bc, &state->bc2);
            coleco_cpu_exchange16(&state->de, &state->de2);
            coleco_cpu_exchange16(&state->hl, &state->hl2);
            return 4;
        case 0xDB:
            // IN A,(n): per Z80 spec, flags are NOT affected.
            coleco_cpu_set_a(state, coleco_memory_in(memory, coleco_cpu_fetch8(state, memory)));
            return 11;
        case 0xDE:
            coleco_cpu_do_alu(state, 3, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xE3: {
            const uint16_t memoryValue = coleco_cpu_mem_read16(memory, state->sp);
            coleco_cpu_mem_write16(memory, state->sp, state->hl);
            state->hl = memoryValue;
            return 19;
        }
        case 0xE6:
            coleco_cpu_do_alu(state, 4, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xE9:
            coleco_cpu_restore_cart_boot_mapping(memory, state->hl, state->lastPc);
            if (coleco_cpu_trace_pc(state->lastPc) || coleco_cpu_trace_pc(state->hl)) {
                coleco_cpu_log_flow("JP(HL)", state, memory, state->lastPc, state->hl);
            }
            state->pc = state->hl;
            return 4;
        case 0xEB:
            coleco_cpu_exchange16(&state->de, &state->hl);
            return 4;
        case 0xED:
            return coleco_cpu_step_ed(state, memory);
        case 0xEE:
            coleco_cpu_do_alu(state, 5, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xF3:
            state->iff1    = false;
            state->iff2    = false;
            state->eiDelay = 0u;
            return 4;
        case 0xF6:
            coleco_cpu_do_alu(state, 6, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xF9:
            state->sp = state->hl;
            return 6;
        case 0xFB:
            // EI: enable interrupts after the *next* instruction (Z80 one-instruction delay).
            state->eiDelay = 2u;
            return 4;
        case 0xFE:
            coleco_cpu_do_alu(state, 7, coleco_cpu_fetch8(state, memory));
            return 7;
        case 0xDD: return coleco_cpu_step_xy(state, memory, &state->ix);
        case 0xFD: return coleco_cpu_step_xy(state, memory, &state->iy);
        default:
            coleco_cpu_mark_unsupported(state, opcode);
            return 0;
    }
}

} // namespace

void coleco_cpu_init(ColecoCpuState* state)
{
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
    state->ix = 0xFFFFu;
    state->iy = 0xFFFFu;
    state->runState = ColecoCpuRunState::Running;
}

void coleco_cpu_reset(ColecoCpuState* state, uint16_t resetPc, uint16_t resetSp)
{
    if (!state) {
        return;
    }

    std::memset(state, 0, sizeof(*state));
    state->pc = resetPc;
    state->sp = resetSp;
    state->ix = 0xFFFFu;
    state->iy = 0xFFFFu;
    state->af = 0x0040u;
    state->runState = ColecoCpuRunState::Running;
}

void coleco_cpu_request_irq(ColecoCpuState* state)
{
    if (!state) {
        return;
    }

#if COLECO_CPU_TRACE_ENABLED
    static uint16_t s_irqRequestLogCount = 0u;
    if (s_irqRequestLogCount < 64u &&
        (coleco_cpu_trace_pc(state->pc) || state->halted)) {
        std::printf("[MSX][IRQ] request pc=%04X iff=%u halt=%u im=%u #%u\n",
                    static_cast<unsigned>(state->pc),
                    state->iff1 ? 1u : 0u,
                    state->halted ? 1u : 0u,
                    static_cast<unsigned>(state->im),
                    static_cast<unsigned>(s_irqRequestLogCount));
        ++s_irqRequestLogCount;
    }
#endif

    state->irqPending = true;
}

int coleco_cpu_run_cycles(ColecoCpuState* state, ColecoMemoryState* memory, int cycleBudget)
{
    if (!state || !memory || cycleBudget <= 0) {
        return 0;
    }

    if (state->runState == ColecoCpuRunState::Unsupported || state->runState == ColecoCpuRunState::Faulted) {
        return 0;
    }

    int usedCycles = 0;
    while (usedCycles < cycleBudget) {
        if (state->nmiPending) {
            const int nmiCycles = coleco_cpu_service_nmi(state, memory);
            usedCycles += nmiCycles;
            state->totalCycles += static_cast<uint32_t>(nmiCycles);
            continue;
        }

        if (state->irqPending && state->iff1) {
            const int irqCycles = coleco_cpu_service_irq(state, memory);
            usedCycles += irqCycles;
            state->totalCycles += static_cast<uint32_t>(irqCycles);
            
            continue;
        }

        if (state->halted) {
            state->runState = ColecoCpuRunState::Halted;
            const int burn = cycleBudget - usedCycles;
            usedCycles += burn;
            state->totalCycles += static_cast<uint32_t>(burn);
            
            break;
        }

        state->runState = ColecoCpuRunState::Running;
        const int stepCycles = coleco_cpu_step_opcode(state, memory);
        if (stepCycles <= 0) {
            break;
        }

        // EI takes effect only after the following instruction has fully completed.
        if (state->eiDelay != 0u) {
            state->eiDelay--;
            if (state->eiDelay == 0u) {
                state->iff1 = true;
                state->iff2 = true;
            }
        }

        usedCycles += stepCycles;
        state->totalCycles += static_cast<uint32_t>(stepCycles);
        
    }

    return usedCycles;
}

const char* coleco_cpu_run_state_label(ColecoCpuRunState state)
{
    switch (state) {
        case ColecoCpuRunState::Running:
            return "RUN";
        case ColecoCpuRunState::Halted:
            return "HALT";
        case ColecoCpuRunState::Unsupported:
            return "UNSUP";
        case ColecoCpuRunState::Faulted:
        default:
            return "FAULT";
    }
}

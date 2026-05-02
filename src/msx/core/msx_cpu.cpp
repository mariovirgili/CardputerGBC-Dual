#include "msx_cpu.h"

#include <esp_attr.h>
#include <cstdio>
#include <cstring>
#include "../msx_media.h"
#include "msx_psg.h"
#include "msx_scc.h"
#include "msx_vdp.h"

#ifndef MSX_CPU_TRACE_ENABLED
#define MSX_CPU_TRACE_ENABLED 0
#endif

#ifndef MSX_CPU_BIOS_CALL_LOG_ENABLED
#define MSX_CPU_BIOS_CALL_LOG_ENABLED 0
#endif

namespace {

constexpr uint8_t kMsxPrimarySlotCartridge = 1u;
constexpr uint8_t kMsxOpenBusFetchOpcodeRet = 0xC9u;
constexpr uint8_t kMsxBootSlotCart = 0xD4u;
constexpr uint8_t kMsxBootSecondaryCart = 0xA0u;
constexpr uint32_t kPsgBatchCycles = 512u;
static uint32_t s_pendingPsgCycles = 0u;

#if defined(__GNUC__)
#define MSX_CPU_FORCE_INLINE inline __attribute__((always_inline))
#define MSX_CPU_LIKELY(expr) __builtin_expect(!!(expr), 1)
#define MSX_CPU_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#else
#define MSX_CPU_FORCE_INLINE inline
#define MSX_CPU_LIKELY(expr) (expr)
#define MSX_CPU_UNLIKELY(expr) (expr)
#endif

constexpr uint8_t kFlagC = 0x01;
constexpr uint8_t kFlagN = 0x02;
constexpr uint8_t kFlagPV = 0x04;
constexpr uint8_t kFlagX = 0x08;
constexpr uint8_t kFlagH = 0x10;
constexpr uint8_t kFlagY = 0x20;
constexpr uint8_t kFlagZ = 0x40;
constexpr uint8_t kFlagS = 0x80;

inline uint8_t msx_hi(uint16_t value)
{
    return static_cast<uint8_t>(value >> 8);
}

inline uint8_t msx_lo(uint16_t value)
{
    return static_cast<uint8_t>(value & 0x00FFu);
}

inline void msx_set_hi(uint16_t* value, uint8_t hi)
{
    *value = static_cast<uint16_t>((*value & 0x00FFu) | (static_cast<uint16_t>(hi) << 8));
}

inline void msx_set_lo(uint16_t* value, uint8_t lo)
{
    *value = static_cast<uint16_t>((*value & 0xFF00u) | lo);
}

inline uint8_t msx_cpu_a(const MsxCpuState* state)
{
    return msx_hi(state->af);
}

inline uint8_t msx_cpu_f(const MsxCpuState* state)
{
    return msx_lo(state->af);
}

inline void msx_cpu_set_a(MsxCpuState* state, uint8_t value)
{
    msx_set_hi(&state->af, value);
}

inline void msx_cpu_set_f(MsxCpuState* state, uint8_t value)
{
    msx_set_lo(&state->af, value);
}

inline bool msx_parity_even(uint8_t value)
{
    value ^= static_cast<uint8_t>(value >> 4);
    value &= 0x0Fu;
    return ((0x6996u >> value) & 0x01u) == 0;
}

inline uint8_t msx_flags_szxy(uint8_t value)
{
    uint8_t flags = static_cast<uint8_t>(value & (kFlagS | kFlagX | kFlagY));
    if (value == 0) {
        flags |= kFlagZ;
    }
    return flags;
}

inline uint8_t msx_flags_szpxy(uint8_t value)
{
    uint8_t flags = msx_flags_szxy(value);
    if (msx_parity_even(value)) {
        flags |= kFlagPV;
    }
    return flags;
}

inline int8_t msx_signed_offset(uint8_t value)
{
    return static_cast<int8_t>(value);
}

inline void IRAM_ATTR msx_cpu_flush_pending_psg_impl(MsxMemoryState* memory)
{
    if (s_pendingPsgCycles == 0u) {
        return;
    }

    const uint32_t cycles = s_pendingPsgCycles;
    s_pendingPsgCycles = 0u;
    if (memory && memory->psg) {
        msx_psg_run_cycles(memory->psg, cycles);
    }
    if (memory) {
        MsxSccState* const scc = msx_memory_get_scc(memory);
        if (scc) {
            msx_scc_run_cycles(scc, cycles);
        }
    }
}

inline void IRAM_ATTR msx_cpu_accumulate_psg_cycles(MsxMemoryState* memory, uint32_t cycles)
{
    if (cycles == 0u) {
        return;
    }

    s_pendingPsgCycles += cycles;
    if (s_pendingPsgCycles >= kPsgBatchCycles) {
        msx_cpu_flush_pending_psg_impl(memory);
    }
}

#if MSX_CPU_TRACE_ENABLED
inline bool msx_cpu_trace_pc(uint16_t pc)
{
    return ((pc >= 0x3F18u) && (pc <= 0x3F30u)) ||
           ((pc >= 0x4100u) && (pc <= 0x423Fu)) ||
           ((pc >= 0x47E0u) && (pc <= 0x47FFu)) ||
           ((pc >= 0x7D0Du) && (pc <= 0x7D10u)) ||
           ((pc >= 0x8170u) && (pc <= 0x8190u)) ||
           (pc == 0xFD9Au);
}

inline bool msx_cpu_trace_opcode_pc(uint16_t pc)
{
    return ((pc >= 0x3F18u) && (pc <= 0x3F30u)) ||
           ((pc >= 0x4100u) && (pc <= 0x423Fu)) ||
           ((pc >= 0x47E0u) && (pc <= 0x47FFu)) ||
           ((pc >= 0x7D0Du) && (pc <= 0x7D10u)) ||
           ((pc >= 0x8170u) && (pc <= 0x8190u)) ||
           (pc == 0xFD9Au);
}

inline bool msx_cpu_trace_addr(uint16_t address)
{
    return (address >= 0xEFF0u) && (address <= 0xF020u);
}

inline bool msx_cpu_trace_cart_probe_addr(uint16_t address)
{
    return (address <= 0x0007u) ||
           ((address >= 0x4000u) && (address <= 0x4007u)) ||
           ((address >= 0x8000u) && (address <= 0x8007u));
}

inline bool msx_cpu_trace_workarea_addr(uint16_t address)
{
    return (address >= 0xF7C5u) && (address <= 0xF7C8u);
}

inline bool msx_cpu_trace_context_pc(uint16_t pc)
{
    return (pc == 0x0038u) ||
           (pc == 0x3F18u) ||
           (pc == 0x3F23u) ||
           (pc == 0x3F24u) ||
           (pc == 0x410Cu) ||
           (pc == 0x4120u) ||
           (pc == 0x41D1u) ||
           (pc == 0x41D4u) ||
           (pc == 0x47E4u) ||
           (pc == 0x47ECu) ||
           (pc == 0x47F7u) ||
           (pc == 0x7D0Du) ||
           (pc == 0x7D10u) ||
           (pc == 0x8132u) ||
           (pc == 0x8175u) ||
           (pc == 0xFD9Au);
}

inline bool msx_cpu_trace_stack_value(uint16_t value)
{
    return ((value >= 0x3F18u) && (value <= 0x3F30u)) ||
           ((value >= 0x4100u) && (value <= 0x423Fu)) ||
           ((value >= 0x7D0Du) && (value <= 0x7D10u)) ||
           ((value >= 0x8170u) && (value <= 0x8190u)) ||
           (value == 0x0038u) ||
           (value == 0xFD9Au);
}
#else
inline bool msx_cpu_trace_pc(uint16_t)
{
    return false;
}

inline bool msx_cpu_trace_opcode_pc(uint16_t)
{
    return false;
}

inline bool msx_cpu_trace_addr(uint16_t)
{
    return false;
}

inline bool msx_cpu_trace_cart_probe_addr(uint16_t)
{
    return false;
}

inline bool msx_cpu_trace_workarea_addr(uint16_t)
{
    return false;
}

inline bool msx_cpu_trace_context_pc(uint16_t)
{
    return false;
}

inline bool msx_cpu_trace_stack_value(uint16_t)
{
    return false;
}
#endif

#if MSX_CPU_BIOS_CALL_LOG_ENABLED
const char* msx_cpu_bios_entry_label(uint16_t address)
{
    switch (address) {
        case 0x001Cu: return "CALSLT";
        case 0x0024u: return "ENASLT";
        case 0x0030u: return "RST30";
        case 0x0047u: return "WRTVDP";
        case 0x004Au: return "RDVRM";
        case 0x004Du: return "WRTVRM";
        case 0x0050u: return "SETRD";
        case 0x0053u: return "SETWRT";
        case 0x0056u: return "FILVRM";
        case 0x0059u: return "LDIRMV";
        case 0x005Cu: return "LDIRVM";
        case 0x005Fu: return "CHGMOD";
        case 0x0062u: return "CHGCLR";
        case 0x0069u: return "CLRSPR";
        case 0x006Cu: return "INITXT";
        case 0x006Fu: return "INIT32";
        case 0x0072u: return "INIGRP";
        case 0x0075u: return "SETTXT";
        case 0x0078u: return "SETT32";
        case 0x007Bu: return "SETGRP";
        case 0x00A2u: return "CHPUT";
        default:
            return nullptr;
    }
}

bool msx_cpu_should_log_bios_entry(uint16_t address)
{
    return msx_cpu_bios_entry_label(address) != nullptr;
}

void msx_cpu_log_bios_entry(const char* kind,
                            const MsxCpuState* state,
                            const MsxMemoryState* memory,
                            uint16_t from,
                            uint16_t to,
                            uint16_t returnAddress)
{
    if (!kind || !state || !memory || !msx_cpu_should_log_bios_entry(to)) {
        return;
    }

    static uint32_t s_biosCallLogCount = 0u;
    if (s_biosCallLogCount >= 256u) {
        return;
    }

    const char* const label = msx_cpu_bios_entry_label(to);
    std::printf("[MSX][BIOS-CALL] %s %s from=%04X to=%04X ret=%04X sp=%04X af=%04X bc=%04X de=%04X hl=%04X A8=%02X SSL3=%02X #%lu\n",
                kind,
                label ? label : "?",
                static_cast<unsigned>(from),
                static_cast<unsigned>(to),
                static_cast<unsigned>(returnAddress),
                static_cast<unsigned>(state->sp),
                static_cast<unsigned>(state->af),
                static_cast<unsigned>(state->bc),
                static_cast<unsigned>(state->de),
                static_cast<unsigned>(state->hl),
                static_cast<unsigned>(memory->slotRegister),
                static_cast<unsigned>(memory->secondarySlotRegs[3]),
                static_cast<unsigned long>(s_biosCallLogCount));
    ++s_biosCallLogCount;
}

void msx_cpu_log_rst30(const MsxCpuState* state,
                       const MsxMemoryState* memory,
                       uint16_t from,
                       uint16_t returnAddress)
{
    if (!state || !memory) {
        return;
    }

    static uint32_t s_rst30LogCount = 0u;
    if (s_rst30LogCount >= 96u) {
        return;
    }

    const uint8_t b0 = msx_memory_read8(memory, returnAddress);
    const uint8_t b1 = msx_memory_read8(memory, static_cast<uint16_t>(returnAddress + 1u));
    const uint8_t b2 = msx_memory_read8(memory, static_cast<uint16_t>(returnAddress + 2u));
    std::printf("[MSX][BIOS-CALL] RST30 from=%04X ret=%04X raw=%02X %02X %02X sp=%04X af=%04X bc=%04X de=%04X hl=%04X A8=%02X SSL3=%02X #%lu\n",
                static_cast<unsigned>(from),
                static_cast<unsigned>(returnAddress),
                static_cast<unsigned>(b0),
                static_cast<unsigned>(b1),
                static_cast<unsigned>(b2),
                static_cast<unsigned>(state->sp),
                static_cast<unsigned>(state->af),
                static_cast<unsigned>(state->bc),
                static_cast<unsigned>(state->de),
                static_cast<unsigned>(state->hl),
                static_cast<unsigned>(memory->slotRegister),
                static_cast<unsigned>(memory->secondarySlotRegs[3]),
                static_cast<unsigned long>(s_rst30LogCount));
    ++s_rst30LogCount;
}
#else
const char* msx_cpu_bios_entry_label(uint16_t)
{
    return nullptr;
}

void msx_cpu_log_bios_entry(const char*,
                            const MsxCpuState*,
                            const MsxMemoryState*,
                            uint16_t,
                            uint16_t,
                            uint16_t)
{
}

void msx_cpu_log_rst30(const MsxCpuState*,
                       const MsxMemoryState*,
                       uint16_t,
                       uint16_t)
{
}
#endif

inline uint8_t msx_cpu_vdp_reg1(const MsxMemoryState* memory)
{
    return (memory && memory->vdp) ? memory->vdp->regs[1] : 0xFFu;
}

inline uint8_t msx_cpu_ram_segment_for_page(const MsxMemoryState* memory, uint8_t pageIndex)
{
    if (!memory || memory->ramSegmentCount == 0u) {
        return 0u;
    }

    if (memory->mapperEnabled && memory->ramSegmentCount > 4u) {
        return msx_memory_wrap_ram_segment(memory, memory->mapperRegisters[pageIndex & 0x03u]);
    }

    return msx_memory_wrap_ram_segment(memory, pageIndex);
}

inline const uint8_t* msx_cpu_raw_ram_bank_ptr(const MsxMemoryState* memory,
                                               uint8_t pageIndex,
                                               uint8_t subPage)
{
    if (!memory || (pageIndex >= 4u) || (subPage >= 2u) || (memory->ramSegmentCount == 0u)) {
        return nullptr;
    }

    const uint8_t segment = msx_cpu_ram_segment_for_page(memory, pageIndex);
    const uint8_t bankIndex = static_cast<uint8_t>(segment * 2u + subPage);
    if ((segment >= memory->ramSegmentCount) ||
        (bankIndex >= memory->ramBankCount) ||
        (bankIndex >= 16u)) {
        return nullptr;
    }

    return memory->ramBanks[bankIndex];
}

inline uint8_t msx_cpu_raw_page3_read8(const MsxMemoryState* memory, uint16_t address, bool* valid = nullptr)
{
    if ((address >> 14) != 3u) {
        if (valid) {
            *valid = false;
        }
        return 0xFFu;
    }

    const uint8_t* const bankPtr = msx_cpu_raw_ram_bank_ptr(
        memory,
        3u,
        static_cast<uint8_t>((address >> 13) & 0x01u)
    );
    if (!bankPtr) {
        if (valid) {
            *valid = false;
        }
        return 0xFFu;
    }

    if (valid) {
        *valid = true;
    }
    return bankPtr[address & 0x1FFFu];
}

inline bool msx_cpu_raw_page3_write8(MsxMemoryState* memory, uint16_t address, uint8_t value)
{
    if (!memory || ((address >> 14) != 3u) || (memory->ramSegmentCount == 0u)) {
        return false;
    }

    const uint8_t segment = msx_cpu_ram_segment_for_page(memory, 3u);
    const uint8_t subPage = static_cast<uint8_t>((address >> 13) & 0x01u);
    const uint8_t bankIndex = static_cast<uint8_t>(segment * 2u + subPage);
    if ((segment >= memory->ramSegmentCount) ||
        (bankIndex >= memory->ramBankCount) ||
        (bankIndex >= 16u) ||
        !memory->ramBanks[bankIndex]) {
        return false;
    }

    memory->ramBanks[bankIndex][address & 0x1FFFu] = value;
    return true;
}

void msx_cpu_log_context(const MsxCpuState* state, const MsxMemoryState* memory, uint16_t pc)
{
    if (!state || !memory || !msx_cpu_trace_context_pc(pc)) {
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
                static_cast<unsigned>(msx_cpu_vdp_reg1(memory)),
                static_cast<unsigned>(memory->slotRegister),
                static_cast<unsigned>(memory->secondarySlotRegs[3]),
                static_cast<unsigned>(s_contextLogCount));
    ++s_contextLogCount;
}

void msx_cpu_log_flow(const char* kind,
                      const MsxCpuState* state,
                      const MsxMemoryState* memory,
                      uint16_t from,
                      uint16_t to)
{
    if (!kind || !state || !memory) {
        return;
    }

    if (!msx_cpu_trace_pc(from) && !msx_cpu_trace_pc(to)) {
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
                static_cast<unsigned>(msx_cpu_vdp_reg1(memory)),
                static_cast<unsigned>(memory->slotRegister),
                static_cast<unsigned>(memory->secondarySlotRegs[3]),
                static_cast<unsigned>(s_flowLogCount));
    ++s_flowLogCount;
}

void msx_cpu_log_stack(const char* kind,
                       const MsxCpuState* state,
                       const MsxMemoryState* memory,
                       uint16_t pc,
                       uint16_t spBefore,
                       uint16_t value)
{
    if (!kind || !state || !memory) {
        return;
    }

    if (!msx_cpu_trace_pc(pc) &&
        !msx_cpu_trace_addr(spBefore) &&
        !msx_cpu_trace_stack_value(value)) {
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
                static_cast<unsigned>(msx_cpu_vdp_reg1(memory)),
                static_cast<unsigned>(memory->slotRegister),
                static_cast<unsigned>(memory->secondarySlotRegs[3]),
                static_cast<unsigned>(s_stackLogCount));
    ++s_stackLogCount;
}

void msx_cpu_log_ram_access(const char* kind,
                            const MsxMemoryState* memory,
                            uint16_t address,
                            uint8_t value)
{
    (void)kind;
    (void)memory;
    (void)address;
    (void)value;
}

void msx_cpu_log_opcode(const MsxCpuState* state,
                        const MsxMemoryState* memory,
                        uint16_t pc,
                        uint8_t opcode)
{
    if (!state || !memory || !msx_cpu_trace_opcode_pc(pc)) {
        return;
    }

    static uint16_t s_opcodeLogCount = 0u;
    if (s_opcodeLogCount >= 512u) {
        return;
    }

    const uint8_t next1 = msx_memory_read8(memory, static_cast<uint16_t>(pc + 1u));
    const uint8_t next2 = msx_memory_read8(memory, static_cast<uint16_t>(pc + 2u));
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
                static_cast<unsigned>(msx_cpu_vdp_reg1(memory)),
                static_cast<unsigned>(memory->slotRegister),
                static_cast<unsigned>(memory->secondarySlotRegs[3]),
                static_cast<unsigned>(s_opcodeLogCount));
    ++s_opcodeLogCount;
}

inline uint8_t IRAM_ATTR msx_cpu_mem_read8(const MsxMemoryState* memory, uint16_t address)
{
    uint8_t mirroredValue = 0xFFu;
    if (address < 0x0010u &&
        msx_memory_try_cart_header_mirror_read(memory, address, &mirroredValue)) {
        msx_cpu_log_ram_access("RD", memory, address, mirroredValue);
        return mirroredValue;
    }

    const uint8_t bank = static_cast<uint8_t>(address >> 13);

    // Page 3 contains the BIOS work area and stack. Only a very small subset
    // of addresses there really need the slow path; the rest can still read
    // directly from the mapped 8K bank pointers.
    if (bank >= 6u) {
        if ((address == 0xFFFFu) || (address == 0xF7C5u) || (address == 0xF7C6u)) {
            const uint8_t value = msx_memory_read8(memory, address);
            msx_cpu_log_ram_access("RD", memory, address, value);
            return value;
        }

        if (msx_memory_use_raw_page3_window(address)) {
            bool valid = false;
            const uint8_t value = msx_cpu_raw_page3_read8(memory, address, &valid);
            if (valid) {
                msx_cpu_log_ram_access("RD", memory, address, value);
                return value;
            }
        }

        const uint8_t value = memory->readMap[bank][address & 0x1FFFu];
        msx_cpu_log_ram_access("RD", memory, address, value);
        return value;
    }

    if (memory &&
        memory->cart.type == MsxCartridgeType::KonamiScc &&
        ((address >= 0x9800u && address < 0xA000u) ||
         (address >= 0xB800u && address < 0xC000u))) {
        const uint8_t value = msx_memory_read8(memory, address);
        msx_cpu_log_ram_access("RD", memory, address, value);
        return value;
    }

    // Match fMSX RdZ80/WrZ80 fast path split for the truly special mirrored
    // addresses used by extension ROM / DiskROM glue.
    if ((address & 0x3F88u) == 0x3F88u) {
        const uint8_t value = msx_memory_read8(memory, address);
        msx_cpu_log_ram_access("RD", memory, address, value);
        return value;
    }

    const uint8_t page = static_cast<uint8_t>(address >> 14);
    const uint8_t slot = static_cast<uint8_t>((memory->slotRegister >> (page * 2u)) & 0x03u);
    if (slot == kMsxPrimarySlotCartridge) {
        uint8_t cartSramValue = 0xFFu;
        if (msx_cart_read_sram(&memory->cart, address, &cartSramValue)) {
            msx_cpu_log_ram_access("RD", memory, address, cartSramValue);
            return cartSramValue;
        }
    }

    const uint8_t value = memory->readMap[bank][address & 0x1FFFu];
    msx_cpu_log_ram_access("RD", memory, address, value);
    return value;
}

inline bool msx_cpu_fetch_can_use_direct_map(const MsxMemoryState* memory, uint16_t address)
{
    if (address >= 0x4000u && address < 0xC000u) {
        if (memory &&
            memory->cart.type == MsxCartridgeType::KonamiScc &&
            ((address >= 0x9800u && address < 0xA000u) ||
             (address >= 0xB800u && address < 0xC000u))) {
            return false;
        }
        return (address & 0x3F88u) != 0x3F88u;
    }

    if (address >= 0xC000u) {
        if ((address == 0xFFFFu) || (address == 0xF7C5u) || (address == 0xF7C6u)) {
            return false;
        }
        if (address >= 0xF000u) {
            if (address >= 0xFE00u && address < 0xFE30u) return true;
            return false;
        }
        return (address & 0x3F88u) != 0x3F88u;
    }

    uint8_t mirroredValue = 0xFFu;
    if (address < 0x0010u && msx_memory_try_cart_header_mirror_read(memory, address, &mirroredValue)) {
        return false;
    }
    return (address & 0x3F88u) != 0x3F88u;
}

inline void msx_cpu_invalidate_fetch_ptr(MsxCpuState* state)
{
    if (!state) {
        return;
    }

    state->fetchBank = 0xFFu;
    state->fetchMapEpoch = 0u;
    state->currentPcAddress = 0u;
    state->currentPcPtr = nullptr;
}

inline void msx_cpu_rebase_fetch_ptr(MsxCpuState* state,
                                     const MsxMemoryState* memory,
                                     uint16_t address)
{
    if (!state || !memory) {
        return;
    }

    const uint8_t bank = static_cast<uint8_t>(address >> 13);
    state->fetchBank = bank;
    state->fetchMapEpoch = memory->mapEpoch;
    state->currentPcAddress = address;
    state->currentPcPtr = memory->readMap[bank] + (address & 0x1FFFu);
}

inline bool msx_cpu_fetch_should_return_open_bus_ret(const MsxMemoryState* memory, uint16_t address)
{
    return memory && msx_memory_is_open_bus_fetch(memory, address);
}

inline bool msx_cpu_is_cart_boot_target(const MsxMemoryState* memory, uint16_t target)
{
    return memory &&
           memory->ready &&
           memory->cart.ready &&
           memory->cart.directBootCandidate &&
           memory->cartBootMappingRestoreArmed &&
           (target >= 0x4000u) &&
           (target < 0xC000u) &&
           (memory->cart.initAddress == target);
}

void msx_cpu_restore_cart_boot_mapping(MsxMemoryState* memory, uint16_t target, uint16_t fromPc)
{
    if (!msx_cpu_is_cart_boot_target(memory, target)) {
        return;
    }

    // The BIOS-to-cart init handoff is a one-shot bootstrap assist. After the
    // first jump into the cartridge, the same work-area bytes can be reused by
    // the game and must no longer trigger a synthetic 402A restart.
    memory->cartBootMappingRestoreArmed = false;
    memory->cartBootWorkareaFallbackArmed = false;

    const uint8_t oldSlotRegister = memory->slotRegister;
    const uint8_t oldSecondary3 = memory->secondarySlotRegs[3];
    if ((oldSlotRegister == kMsxBootSlotCart) && (oldSecondary3 == kMsxBootSecondaryCart)) {
        return;
    }

    memory->slotRegister = kMsxBootSlotCart;
    memory->secondarySlotRegs[3] = kMsxBootSecondaryCart;
    memory->lastPortA8 = kMsxBootSlotCart;
    msx_memory_refresh_maps(memory);

#if MSX_CPU_TRACE_ENABLED
    static uint16_t s_cartBootMapRestoreLogCount = 0u;
    if (s_cartBootMapRestoreLogCount < 16u) {
        std::printf("[MSX][BOOTMAP] restore jump=%04X from=%04X A8=%02X->%02X SSL3=%02X->%02X #%u\n",
                    static_cast<unsigned>(target),
                    static_cast<unsigned>(fromPc),
                    static_cast<unsigned>(oldSlotRegister),
                    static_cast<unsigned>(kMsxBootSlotCart),
                    static_cast<unsigned>(oldSecondary3),
                    static_cast<unsigned>(kMsxBootSecondaryCart),
                    static_cast<unsigned>(s_cartBootMapRestoreLogCount));
        ++s_cartBootMapRestoreLogCount;
    }
#endif
}

inline uint16_t msx_cpu_mem_read16(const MsxMemoryState* memory, uint16_t address)
{
    const uint8_t lo = msx_cpu_mem_read8(memory, address);
    const uint8_t hi = msx_cpu_mem_read8(memory, static_cast<uint16_t>(address + 1u));
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

inline void msx_cpu_mem_write8(MsxMemoryState* memory, uint16_t address, uint8_t value)
{
    const uint8_t bank = static_cast<uint8_t>(address >> 13);

    if (bank >= 6u) {
        if (address == 0xFFFFu) {
            msx_memory_write8(memory, address, value);
            msx_cpu_log_ram_access("WR", memory, address, value);
            return;
        }

        if (msx_memory_use_raw_page3_window(address) &&
            msx_cpu_raw_page3_write8(memory, address, value)) {
            msx_cpu_log_ram_access("WR", memory, address, value);
            return;
        }

        const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);
        uint8_t* const writePage = memory->writeMap[bank];
        if (writePage) {
            writePage[offset] = value;
            msx_cpu_log_ram_access("WR", memory, address, value);
            return;
        }

        msx_memory_write8(memory, address, value);
        msx_cpu_log_ram_access("WR", memory, address, value);
        return;
    }

    if ((address & 0x3F88u) == 0x3F88u) {
        msx_memory_write8(memory, address, value);
        msx_cpu_log_ram_access("WR", memory, address, value);
        return;
    }

    const uint16_t offset = static_cast<uint16_t>(address & 0x1FFFu);
    uint8_t* const writePage = memory->writeMap[bank];
    if (writePage) {
        writePage[offset] = value;
        msx_cpu_log_ram_access("WR", memory, address, value);
        return;
    }

    const uint8_t page = static_cast<uint8_t>(address >> 14);
    const uint8_t slot = static_cast<uint8_t>((memory->slotRegister >> (page * 2u)) & 0x03u);
    if (slot == kMsxPrimarySlotCartridge) {
        msx_memory_write8(memory, address, value);
    }
    msx_cpu_log_ram_access("WR", memory, address, value);
}

inline void msx_cpu_mem_write16(MsxMemoryState* memory, uint16_t address, uint16_t value)
{
    msx_cpu_mem_write8(memory, address, static_cast<uint8_t>(value & 0xFFu));
    msx_cpu_mem_write8(memory, static_cast<uint16_t>(address + 1u), static_cast<uint8_t>(value >> 8));
}

MSX_CPU_FORCE_INLINE uint8_t IRAM_ATTR msx_cpu_get_reg8(const MsxCpuState* state, const MsxMemoryState* memory, uint8_t reg)
{
    switch (reg & 0x07u) {
        case 0: return msx_hi(state->bc);
        case 1: return msx_lo(state->bc);
        case 2: return msx_hi(state->de);
        case 3: return msx_lo(state->de);
        case 4: return msx_hi(state->hl);
        case 5: return msx_lo(state->hl);
        case 6: return msx_cpu_mem_read8(memory, state->hl);
        default: return msx_cpu_a(state);
    }
}

MSX_CPU_FORCE_INLINE void IRAM_ATTR msx_cpu_set_reg8(MsxCpuState* state, MsxMemoryState* memory, uint8_t reg, uint8_t value)
{
    switch (reg & 0x07u) {
        case 0: msx_set_hi(&state->bc, value); break;
        case 1: msx_set_lo(&state->bc, value); break;
        case 2: msx_set_hi(&state->de, value); break;
        case 3: msx_set_lo(&state->de, value); break;
        case 4: msx_set_hi(&state->hl, value); break;
        case 5: msx_set_lo(&state->hl, value); break;
        case 6: msx_cpu_mem_write8(memory, state->hl, value); break;
        default: msx_cpu_set_a(state, value); break;
    }
}

MSX_CPU_FORCE_INLINE uint16_t* IRAM_ATTR msx_cpu_reg16_ptr(MsxCpuState* state, uint8_t pair)
{
    switch (pair & 0x03u) {
        case 0: return &state->bc;
        case 1: return &state->de;
        case 2: return &state->hl;
        default: return &state->sp;
    }
}

MSX_CPU_FORCE_INLINE uint16_t* IRAM_ATTR msx_cpu_stack_reg16_ptr(MsxCpuState* state, uint8_t pair)
{
    switch (pair & 0x03u) {
        case 0: return &state->bc;
        case 1: return &state->de;
        case 2: return &state->hl;
        default: return &state->af;
    }
}

uint16_t IRAM_ATTR msx_cpu_fetch16(MsxCpuState* state, const MsxMemoryState* memory);

// Forward declaration: msx_cpu_step_xy's default case re-dispatches here
int IRAM_ATTR msx_cpu_step_opcode(MsxCpuState* state, MsxMemoryState* memory);

uint8_t IRAM_ATTR msx_cpu_fetch8(MsxCpuState* state, const MsxMemoryState* memory)
{
    const uint16_t pc = state->pc;
    uint8_t value;

    if (MSX_CPU_LIKELY(state->currentPcAddress == pc && state->fetchMapEpoch == memory->mapEpoch)) {
        value = *state->currentPcPtr++;
        state->currentPcAddress = static_cast<uint16_t>(pc + 1u);
    } else {
        if (MSX_CPU_LIKELY(msx_cpu_fetch_can_use_direct_map(memory, pc))) {
            msx_cpu_rebase_fetch_ptr(state, memory, pc);
            value = *state->currentPcPtr++;
            state->currentPcAddress = static_cast<uint16_t>(pc + 1u);
        } else {
            value = msx_cpu_mem_read8(memory, pc);
            msx_cpu_invalidate_fetch_ptr(state);
        }
    }

    if (MSX_CPU_UNLIKELY(value == 0xFFu) && msx_cpu_fetch_should_return_open_bus_ret(memory, pc)) {
        value = kMsxOpenBusFetchOpcodeRet;
    }
    state->pc = static_cast<uint16_t>(pc + 1u);
    state->r = static_cast<uint8_t>(state->r + 1u);
    return value;
}

uint16_t IRAM_ATTR msx_cpu_fetch16(MsxCpuState* state, const MsxMemoryState* memory)
{
    const uint8_t lo = msx_cpu_fetch8(state, memory);
    const uint8_t hi = msx_cpu_fetch8(state, memory);
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

void msx_cpu_push16(MsxCpuState* state, MsxMemoryState* memory, uint16_t value)
{
    const uint16_t spBefore = state->sp;
    state->sp = static_cast<uint16_t>(state->sp - 2u);
    msx_cpu_mem_write16(memory, state->sp, value);
    msx_cpu_log_stack("PUSH", state, memory, state->lastPc, spBefore, value);
}

uint16_t msx_cpu_pop16(MsxCpuState* state, const MsxMemoryState* memory)
{
    const uint16_t spBefore = state->sp;
    const uint16_t value = msx_cpu_mem_read16(memory, state->sp);
    state->sp = static_cast<uint16_t>(state->sp + 2u);
    msx_cpu_log_stack("POP", state, memory, state->lastPc, spBefore, value);
    return value;
}

MSX_CPU_FORCE_INLINE bool IRAM_ATTR msx_cpu_condition(const MsxCpuState* state, uint8_t condition)
{
    const uint8_t flags = msx_cpu_f(state);
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

void msx_cpu_exchange16(uint16_t* lhs, uint16_t* rhs)
{
    const uint16_t value = *lhs;
    *lhs = *rhs;
    *rhs = value;
}

uint8_t msx_cpu_inc8(MsxCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(value + 1u);
    uint8_t flags = static_cast<uint8_t>(msx_cpu_f(state) & kFlagC);
    flags |= msx_flags_szxy(result);
    if ((value & 0x0Fu) == 0x0Fu) {
        flags |= kFlagH;
    }
    if (value == 0x7Fu) {
        flags |= kFlagPV;
    }
    msx_cpu_set_f(state, flags);
    return result;
}

uint8_t msx_cpu_dec8(MsxCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(value - 1u);
    uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | kFlagN);
    flags |= msx_flags_szxy(result);
    if ((value & 0x0Fu) == 0x00u) {
        flags |= kFlagH;
    }
    if (value == 0x80u) {
        flags |= kFlagPV;
    }
    msx_cpu_set_f(state, flags);
    return result;
}

uint8_t msx_cpu_add8(MsxCpuState* state, uint8_t lhs, uint8_t rhs, uint8_t carry)
{
    const uint16_t full = static_cast<uint16_t>(lhs) + static_cast<uint16_t>(rhs) + static_cast<uint16_t>(carry);
    const uint8_t result = static_cast<uint8_t>(full & 0x00FFu);
    uint8_t flags = msx_flags_szxy(result);
    if (((lhs & 0x0Fu) + (rhs & 0x0Fu) + carry) > 0x0Fu) {
        flags |= kFlagH;
    }
    if (((~(lhs ^ rhs) & (lhs ^ result)) & 0x80u) != 0) {
        flags |= kFlagPV;
    }
    if ((full & 0x0100u) != 0) {
        flags |= kFlagC;
    }
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, flags);
    return result;
}

uint8_t msx_cpu_sub8(MsxCpuState* state, uint8_t lhs, uint8_t rhs, uint8_t carry)
{
    const uint16_t full = static_cast<uint16_t>(lhs) - static_cast<uint16_t>(rhs) - static_cast<uint16_t>(carry);
    const uint8_t result = static_cast<uint8_t>(full & 0x00FFu);
    uint8_t flags = static_cast<uint8_t>(msx_flags_szxy(result) | kFlagN);
    if ((static_cast<uint16_t>(lhs & 0x0Fu) - static_cast<uint16_t>(rhs & 0x0Fu) - carry) & 0x0010u) {
        flags |= kFlagH;
    }
    if ((((lhs ^ rhs) & (lhs ^ result)) & 0x80u) != 0) {
        flags |= kFlagPV;
    }
    if ((full & 0x0100u) != 0) {
        flags |= kFlagC;
    }
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, flags);
    return result;
}

void msx_cpu_logic_and(MsxCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(msx_cpu_a(state) & value);
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, static_cast<uint8_t>(msx_flags_szpxy(result) | kFlagH));
}

void msx_cpu_logic_xor(MsxCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(msx_cpu_a(state) ^ value);
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, msx_flags_szpxy(result));
}

void msx_cpu_logic_or(MsxCpuState* state, uint8_t value)
{
    const uint8_t result = static_cast<uint8_t>(msx_cpu_a(state) | value);
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, msx_flags_szpxy(result));
}

void msx_cpu_compare8(MsxCpuState* state, uint8_t value)
{
    const uint8_t accumulator = msx_cpu_a(state);
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
    msx_cpu_set_f(state, flags);
}

void msx_cpu_add16_hl(MsxCpuState* state, uint16_t value)
{
    const uint16_t lhs = state->hl;
    const uint32_t full = static_cast<uint32_t>(lhs) + static_cast<uint32_t>(value);
    const uint16_t result = static_cast<uint16_t>(full & 0xFFFFu);
    uint8_t flags = static_cast<uint8_t>(msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV));
    flags |= static_cast<uint8_t>((result >> 8) & (kFlagX | kFlagY));
    if (((lhs & 0x0FFFu) + (value & 0x0FFFu)) > 0x0FFFu) {
        flags |= kFlagH;
    }
    if ((full & 0x10000u) != 0) {
        flags |= kFlagC;
    }
    state->hl = result;
    msx_cpu_set_f(state, flags);
}

void msx_cpu_adc16_hl(MsxCpuState* state, uint16_t value)
{
    const uint32_t lhs = state->hl;
    const uint32_t carry = (msx_cpu_f(state) & kFlagC) ? 1u : 0u;
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
    msx_cpu_set_f(state, flags);
}

void msx_cpu_sbc16_hl(MsxCpuState* state, uint16_t value)
{
    const uint32_t lhs = state->hl;
    const uint32_t carry = (msx_cpu_f(state) & kFlagC) ? 1u : 0u;
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
    msx_cpu_set_f(state, flags);
}

void msx_cpu_daa(MsxCpuState* state)
{
    const uint8_t oldA = msx_cpu_a(state);
    uint8_t adjust = 0;
    uint8_t flags = msx_cpu_f(state);
    bool carry = (flags & kFlagC) != 0;

    if ((flags & kFlagN) == 0) {
        if ((flags & kFlagH) != 0 || (oldA & 0x0Fu) > 0x09u) {
            adjust |= 0x06u;
        }
        if (carry || oldA > 0x99u) {
            adjust |= 0x60u;
            carry = true;
        }
        msx_cpu_set_a(state, static_cast<uint8_t>(oldA + adjust));
    } else {
        if ((flags & kFlagH) != 0) {
            adjust |= 0x06u;
        }
        if (carry) {
            adjust |= 0x60u;
        }
        msx_cpu_set_a(state, static_cast<uint8_t>(oldA - adjust));
    }

    const uint8_t result = msx_cpu_a(state);
    uint8_t newFlags = static_cast<uint8_t>(flags & kFlagN);
    newFlags |= msx_flags_szpxy(result);
    if ((((oldA ^ result) ^ adjust) & 0x10u) != 0) {
        newFlags |= kFlagH;
    }
    if (carry) {
        newFlags |= kFlagC;
    }
    msx_cpu_set_f(state, newFlags);
}

void msx_cpu_acc_rotate_left_carry(MsxCpuState* state)
{
    const uint8_t value = msx_cpu_a(state);
    const uint8_t result = static_cast<uint8_t>((value << 1) | (value >> 7));
    uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x80u) {
        flags |= kFlagC;
    }
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, flags);
}

void msx_cpu_acc_rotate_right_carry(MsxCpuState* state)
{
    const uint8_t value = msx_cpu_a(state);
    const uint8_t result = static_cast<uint8_t>((value >> 1) | (value << 7));
    uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x01u) {
        flags |= kFlagC;
    }
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, flags);
}

void msx_cpu_acc_rotate_left(MsxCpuState* state)
{
    const uint8_t value = msx_cpu_a(state);
    const uint8_t carry = (msx_cpu_f(state) & kFlagC) ? 1u : 0u;
    const uint8_t result = static_cast<uint8_t>((value << 1) | carry);
    uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x80u) {
        flags |= kFlagC;
    }
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, flags);
}

void msx_cpu_acc_rotate_right(MsxCpuState* state)
{
    const uint8_t value = msx_cpu_a(state);
    const uint8_t carry = (msx_cpu_f(state) & kFlagC) ? 0x80u : 0u;
    const uint8_t result = static_cast<uint8_t>((value >> 1) | carry);
    uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) | (result & (kFlagX | kFlagY)));
    if (value & 0x01u) {
        flags |= kFlagC;
    }
    msx_cpu_set_a(state, result);
    msx_cpu_set_f(state, flags);
}

void msx_cpu_set_in_flags(MsxCpuState* state, uint8_t value)
{
    msx_cpu_set_f(state, static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | msx_flags_szpxy(value)));
}

void msx_cpu_mark_unsupported(MsxCpuState* state, uint8_t opcode)
{
    state->unsupportedOpcode = opcode;
    state->unsupportedPc = state->lastPc;
    state->runState = MsxCpuRunState::Unsupported;
}

int msx_cpu_service_irq(MsxCpuState* state, MsxMemoryState* memory)
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
    state->runState = MsxCpuRunState::Running;
    #if MSX_CPU_TRACE_ENABLED
    static uint16_t s_haltWakeLogCount = 0u;
    if (wasHalted && s_haltWakeLogCount < 8u) {
        const uint8_t resume0 = msx_memory_read8(memory, state->pc);
        const uint8_t resume1 = msx_memory_read8(memory, static_cast<uint16_t>(state->pc + 1u));
        const uint8_t resume2 = msx_memory_read8(memory, static_cast<uint16_t>(state->pc + 2u));
        const uint8_t hook0 = msx_memory_read8(memory, 0xFD9Au);
        const uint8_t hook1 = msx_memory_read8(memory, 0xFD9Bu);
        const uint8_t hook2 = msx_memory_read8(memory, 0xFD9Cu);
        std::printf("[MSX][HALT] wake at=%04X resume=%04X im=%u A8=%02X SSL3=%02X cart=%s banks=%u/%u/%u/%u bytes=%02X %02X %02X hook=%02X %02X %02X #%u\n",
                    static_cast<unsigned>(state->lastPc),
                    static_cast<unsigned>(state->pc),
                    static_cast<unsigned>(state->im),
                    static_cast<unsigned>(memory->slotRegister),
                    static_cast<unsigned>(memory->secondarySlotRegs[3]),
                    msx_media_cartridge_type_label(memory->cart.type),
                    static_cast<unsigned>(memory->cart.windowBanks[0]),
                    static_cast<unsigned>(memory->cart.windowBanks[1]),
                    static_cast<unsigned>(memory->cart.windowBanks[2]),
                    static_cast<unsigned>(memory->cart.windowBanks[3]),
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
    msx_cpu_push16(state, memory, state->pc);

    if (state->im == 2u) {
        // IM 2: vector table at (I << 8) | 0xFF
        const uint16_t vecAddr = static_cast<uint16_t>((static_cast<uint16_t>(state->i) << 8) | 0xFFu);
        state->pc = msx_cpu_mem_read16(memory, vecAddr);
        if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc) || msx_cpu_trace_addr(state->sp)) {
            msx_cpu_log_flow("IRQ2", state, memory, state->lastPc, state->pc);
        }
        return 19;
    }

    // IM 0 / IM 1: jump to 0x0038
    state->pc = 0x0038u;
    if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc) || msx_cpu_trace_addr(state->sp)) {
        msx_cpu_log_flow("IRQ", state, memory, state->lastPc, state->pc);
    }
    return 13;
}

void msx_cpu_do_alu(MsxCpuState* state, uint8_t aluOp, uint8_t value)
{
    switch (aluOp & 0x07u) {
        case 0: msx_cpu_add8(state, msx_cpu_a(state), value, 0); break;
        case 1: msx_cpu_add8(state, msx_cpu_a(state), value, (msx_cpu_f(state) & kFlagC) ? 1u : 0u); break;
        case 2: msx_cpu_sub8(state, msx_cpu_a(state), value, 0); break;
        case 3: msx_cpu_sub8(state, msx_cpu_a(state), value, (msx_cpu_f(state) & kFlagC) ? 1u : 0u); break;
        case 4: msx_cpu_logic_and(state, value); break;
        case 5: msx_cpu_logic_xor(state, value); break;
        case 6: msx_cpu_logic_or(state, value); break;
        default: msx_cpu_compare8(state, value); break;
    }
}

uint8_t msx_cpu_cb_rotate(MsxCpuState* state, uint8_t operation, uint8_t value)
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
            const uint8_t carry = (msx_cpu_f(state) & kFlagC) ? 1u : 0u;
            result = static_cast<uint8_t>((value << 1) | carry);
            if (value & 0x80u) flags |= kFlagC;
            break;
        }
        case 3: {
            const uint8_t carry = (msx_cpu_f(state) & kFlagC) ? 0x80u : 0u;
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

    flags |= msx_flags_szpxy(result);
    msx_cpu_set_f(state, flags);
    return result;
}

// ---- IX / IY prefix helpers (DD / FD) ----
// Based on fMSX Z80 engine by Marat Fayzullin, as ported to ESP32 in esplay-fMSX.
// Adapted to the existing code style: explicit switch dispatch instead of the
// canonical CodesXX.h macro trick, same correctness guarantees.

inline uint8_t msx_cpu_xyh(const uint16_t* xy)
{
    return static_cast<uint8_t>(*xy >> 8);
}

inline uint8_t msx_cpu_xyl(const uint16_t* xy)
{
    return static_cast<uint8_t>(*xy & 0xFFu);
}

inline void msx_cpu_set_xyh(uint16_t* xy, uint8_t v)
{
    *xy = static_cast<uint16_t>((*xy & 0x00FFu) | (static_cast<uint16_t>(v) << 8));
}

inline void msx_cpu_set_xyl(uint16_t* xy, uint8_t v)
{
    *xy = static_cast<uint16_t>((*xy & 0xFF00u) | v);
}

// Consume displacement byte and return effective address (IX+d) or (IY+d).
inline uint16_t msx_cpu_xy_ea(MsxCpuState* state, const MsxMemoryState* memory, const uint16_t* xy)
{
    const int8_t disp = static_cast<int8_t>(msx_cpu_fetch8(state, memory));
    return static_cast<uint16_t>(static_cast<int32_t>(*xy) + static_cast<int32_t>(disp));
}

// ADD IX,rr / ADD IY,rr — same as msx_cpu_add16_hl but uses xy in place of HL.
void msx_cpu_add16_xy(MsxCpuState* state, uint16_t* xy, uint16_t value)
{
    const uint16_t lhs = *xy;
    const uint32_t full = static_cast<uint32_t>(lhs) + static_cast<uint32_t>(value);
    const uint16_t result = static_cast<uint16_t>(full & 0xFFFFu);
    uint8_t flags = static_cast<uint8_t>(msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV));
    flags |= static_cast<uint8_t>((result >> 8) & (kFlagX | kFlagY));
    if (((lhs & 0x0FFFu) + (value & 0x0FFFu)) > 0x0FFFu) {
        flags |= kFlagH;
    }
    if ((full & 0x10000u) != 0u) {
        flags |= kFlagC;
    }
    *xy = result;
    msx_cpu_set_f(state, flags);
}

// DDCB / FDCB: indexed bit operations.
// Encoding: DD CB <disp> <op> — displacement is fetched before the operation opcode.
int IRAM_ATTR msx_cpu_step_xycb(MsxCpuState* state, MsxMemoryState* memory, const uint16_t* xy)
{
    const int8_t  disp   = static_cast<int8_t>(msx_cpu_fetch8(state, memory));
    const uint16_t ea    = static_cast<uint16_t>(static_cast<int32_t>(*xy) + static_cast<int32_t>(disp));
    const uint8_t  opcode = msx_cpu_fetch8(state, memory);
    const uint8_t  group  = static_cast<uint8_t>(opcode >> 6);
    const uint8_t  y      = static_cast<uint8_t>((opcode >> 3) & 0x07u);
    const uint8_t  z      = static_cast<uint8_t>(opcode & 0x07u);
    const uint8_t  value  = msx_cpu_mem_read8(memory, ea);

    if (group == 0u) {
        // Rotate / shift on (IX+d)
        const uint8_t result = msx_cpu_cb_rotate(state, y, value);
        msx_cpu_mem_write8(memory, ea, result);
        if (z != 6u) {
            msx_cpu_set_reg8(state, memory, z, result);
        }
        return 23;
    }

    if (group == 1u) {
        // BIT y,(IX+d)
        uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | kFlagH);
        if ((value & static_cast<uint8_t>(1u << y)) == 0u) {
            flags |= static_cast<uint8_t>(kFlagZ | kFlagPV);
        }
        if ((y == 7u) && ((value & 0x80u) != 0u)) {
            flags |= kFlagS;
        }
        msx_cpu_set_f(state, flags);
        return 20;
    }

    // RES y,(IX+d) or SET y,(IX+d)
    const uint8_t result = (group == 2u)
        ? static_cast<uint8_t>(value & ~static_cast<uint8_t>(1u << y))
        : static_cast<uint8_t>(value |  static_cast<uint8_t>(1u << y));
    msx_cpu_mem_write8(memory, ea, result);
    if (z != 6u) {
        msx_cpu_set_reg8(state, memory, z, result);
    }
    return 23;
}

// DD / FD prefix handler.
// xy points to state->ix (DD) or state->iy (FD).
// For opcodes that have no IX/IY variant the prefix is silently discarded and
// the opcode is re-decoded by the base dispatcher — correct Z80 behaviour.
int IRAM_ATTR msx_cpu_step_xy(MsxCpuState* state, MsxMemoryState* memory, uint16_t* xy)
{
    const uint8_t opcode = msx_cpu_fetch8(state, memory);

    switch (opcode) {
        // 16-bit loads and arithmetic — HL replaced by IX/IY
        case 0x09: msx_cpu_add16_xy(state, xy, state->bc); return 15;
        case 0x19: msx_cpu_add16_xy(state, xy, state->de); return 15;
        case 0x21: *xy = msx_cpu_fetch16(state, memory); return 14;
        case 0x22: { const uint16_t a = msx_cpu_fetch16(state, memory); msx_cpu_mem_write16(memory, a, *xy); return 20; }
        case 0x23: *xy = static_cast<uint16_t>(*xy + 1u); return 10;
        case 0x29: msx_cpu_add16_xy(state, xy, *xy); return 15;
        case 0x2A: { const uint16_t a = msx_cpu_fetch16(state, memory); *xy = msx_cpu_mem_read16(memory, a); return 20; }
        case 0x2B: *xy = static_cast<uint16_t>(*xy - 1u); return 10;
        case 0x39: msx_cpu_add16_xy(state, xy, state->sp); return 15;

        // IXH / IXL operations (documented on real silicon; used by MSX BIOS)
        case 0x24: msx_cpu_set_xyh(xy, msx_cpu_inc8(state, msx_cpu_xyh(xy))); return 4;
        case 0x25: msx_cpu_set_xyh(xy, msx_cpu_dec8(state, msx_cpu_xyh(xy))); return 4;
        case 0x26: msx_cpu_set_xyh(xy, msx_cpu_fetch8(state, memory)); return 7;
        case 0x2C: msx_cpu_set_xyl(xy, msx_cpu_inc8(state, msx_cpu_xyl(xy))); return 4;
        case 0x2D: msx_cpu_set_xyl(xy, msx_cpu_dec8(state, msx_cpu_xyl(xy))); return 4;
        case 0x2E: msx_cpu_set_xyl(xy, msx_cpu_fetch8(state, memory)); return 7;

        // LD r, IXH/IXL
        case 0x44: msx_set_hi(&state->bc, msx_cpu_xyh(xy)); return 4;
        case 0x45: msx_set_hi(&state->bc, msx_cpu_xyl(xy)); return 4;
        case 0x4C: msx_set_lo(&state->bc, msx_cpu_xyh(xy)); return 4;
        case 0x4D: msx_set_lo(&state->bc, msx_cpu_xyl(xy)); return 4;
        case 0x54: msx_set_hi(&state->de, msx_cpu_xyh(xy)); return 4;
        case 0x55: msx_set_hi(&state->de, msx_cpu_xyl(xy)); return 4;
        case 0x5C: msx_set_lo(&state->de, msx_cpu_xyh(xy)); return 4;
        case 0x5D: msx_set_lo(&state->de, msx_cpu_xyl(xy)); return 4;
        case 0x7C: msx_cpu_set_a(state, msx_cpu_xyh(xy)); return 4;
        case 0x7D: msx_cpu_set_a(state, msx_cpu_xyl(xy)); return 4;

        // LD IXH, r
        case 0x60: msx_cpu_set_xyh(xy, msx_hi(state->bc));  return 4;
        case 0x61: msx_cpu_set_xyh(xy, msx_lo(state->bc));  return 4;
        case 0x62: msx_cpu_set_xyh(xy, msx_hi(state->de));  return 4;
        case 0x63: msx_cpu_set_xyh(xy, msx_lo(state->de));  return 4;
        case 0x64: /* LD IXH,IXH */ return 4;
        case 0x65: msx_cpu_set_xyh(xy, msx_cpu_xyl(xy));    return 4;
        case 0x67: msx_cpu_set_xyh(xy, msx_cpu_a(state));   return 4;

        // LD IXL, r
        case 0x68: msx_cpu_set_xyl(xy, msx_hi(state->bc));  return 4;
        case 0x69: msx_cpu_set_xyl(xy, msx_lo(state->bc));  return 4;
        case 0x6A: msx_cpu_set_xyl(xy, msx_hi(state->de));  return 4;
        case 0x6B: msx_cpu_set_xyl(xy, msx_lo(state->de));  return 4;
        case 0x6C: msx_cpu_set_xyl(xy, msx_cpu_xyh(xy));    return 4;
        case 0x6D: /* LD IXL,IXL */ return 4;
        case 0x6F: msx_cpu_set_xyl(xy, msx_cpu_a(state));   return 4;

        // ALU A, IXH
        case 0x84: msx_cpu_do_alu(state, 0, msx_cpu_xyh(xy)); return 4;
        case 0x8C: msx_cpu_do_alu(state, 1, msx_cpu_xyh(xy)); return 4;
        case 0x94: msx_cpu_do_alu(state, 2, msx_cpu_xyh(xy)); return 4;
        case 0x9C: msx_cpu_do_alu(state, 3, msx_cpu_xyh(xy)); return 4;
        case 0xA4: msx_cpu_do_alu(state, 4, msx_cpu_xyh(xy)); return 4;
        case 0xAC: msx_cpu_do_alu(state, 5, msx_cpu_xyh(xy)); return 4;
        case 0xB4: msx_cpu_do_alu(state, 6, msx_cpu_xyh(xy)); return 4;
        case 0xBC: msx_cpu_do_alu(state, 7, msx_cpu_xyh(xy)); return 4;

        // ALU A, IXL
        case 0x85: msx_cpu_do_alu(state, 0, msx_cpu_xyl(xy)); return 4;
        case 0x8D: msx_cpu_do_alu(state, 1, msx_cpu_xyl(xy)); return 4;
        case 0x95: msx_cpu_do_alu(state, 2, msx_cpu_xyl(xy)); return 4;
        case 0x9D: msx_cpu_do_alu(state, 3, msx_cpu_xyl(xy)); return 4;
        case 0xA5: msx_cpu_do_alu(state, 4, msx_cpu_xyl(xy)); return 4;
        case 0xAD: msx_cpu_do_alu(state, 5, msx_cpu_xyl(xy)); return 4;
        case 0xB5: msx_cpu_do_alu(state, 6, msx_cpu_xyl(xy)); return 4;
        case 0xBD: msx_cpu_do_alu(state, 7, msx_cpu_xyl(xy)); return 4;

        // INC / DEC / LD on indexed memory (IX+d)
        case 0x34: {
            const uint16_t ea = msx_cpu_xy_ea(state, memory, xy);
            msx_cpu_mem_write8(memory, ea, msx_cpu_inc8(state, msx_cpu_mem_read8(memory, ea)));
            return 23;
        }
        case 0x35: {
            const uint16_t ea = msx_cpu_xy_ea(state, memory, xy);
            msx_cpu_mem_write8(memory, ea, msx_cpu_dec8(state, msx_cpu_mem_read8(memory, ea)));
            return 23;
        }
        case 0x36: {
            const uint16_t ea  = msx_cpu_xy_ea(state, memory, xy);
            const uint8_t  imm = msx_cpu_fetch8(state, memory);
            msx_cpu_mem_write8(memory, ea, imm);
            return 19;
        }

        // LD r,(IX+d) — destination is always the true register (not IXH/IXL)
        case 0x46: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_set_hi(&state->bc, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x4E: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_set_lo(&state->bc, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x56: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_set_hi(&state->de, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x5E: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_set_lo(&state->de, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x66: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_set_hi(&state->hl, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x6E: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_set_lo(&state->hl, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x7E: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_set_a(state, msx_cpu_mem_read8(memory, ea)); return 19; }

        // LD (IX+d),r — source is always the true register (not IXH/IXL)
        case 0x70: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_mem_write8(memory, ea, msx_hi(state->bc)); return 19; }
        case 0x71: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_mem_write8(memory, ea, msx_lo(state->bc)); return 19; }
        case 0x72: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_mem_write8(memory, ea, msx_hi(state->de)); return 19; }
        case 0x73: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_mem_write8(memory, ea, msx_lo(state->de)); return 19; }
        case 0x74: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_mem_write8(memory, ea, msx_hi(state->hl)); return 19; }
        case 0x75: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_mem_write8(memory, ea, msx_lo(state->hl)); return 19; }
        case 0x77: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_mem_write8(memory, ea, msx_cpu_a(state)); return 19; }

        // ALU A,(IX+d)
        case 0x86: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 0, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x8E: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 1, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x96: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 2, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0x9E: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 3, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0xA6: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 4, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0xAE: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 5, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0xB6: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 6, msx_cpu_mem_read8(memory, ea)); return 19; }
        case 0xBE: { const uint16_t ea = msx_cpu_xy_ea(state, memory, xy); msx_cpu_do_alu(state, 7, msx_cpu_mem_read8(memory, ea)); return 19; }

        // Stack and jump with IX/IY
        case 0xE1: *xy = msx_cpu_pop16(state, memory); return 14;
        case 0xE3: {
            const uint16_t top = msx_cpu_mem_read16(memory, state->sp);
            msx_cpu_mem_write16(memory, state->sp, *xy);
            *xy = top;
            return 23;
        }
        case 0xE5: msx_cpu_push16(state, memory, *xy); return 15;
        case 0xE9:
            msx_cpu_restore_cart_boot_mapping(memory, *xy, state->lastPc);
            state->pc = *xy;
            return 8;
        case 0xF9: state->sp = *xy; return 10;

        // DDCB / FDCB: 4-byte indexed bit operations
        case 0xCB: return msx_cpu_step_xycb(state, memory, xy);

        // Nested DD/FD: second prefix restarts with new register target
        case 0xDD: return msx_cpu_step_xy(state, memory, &state->ix);
        case 0xFD: return msx_cpu_step_xy(state, memory, &state->iy);

        // Unrecognised opcode after DD/FD: prefix is ignored on real Z80.
        // Undo the fetch so the base dispatcher sees the opcode unmodified.
        default:
            state->pc = static_cast<uint16_t>(state->pc - 1u);
            state->r  = static_cast<uint8_t>(state->r  - 1u);
            return msx_cpu_step_opcode(state, memory);
    }
}

int IRAM_ATTR msx_cpu_step_cb(MsxCpuState* state, MsxMemoryState* memory)
{
    const uint8_t opcode = msx_cpu_fetch8(state, memory);
    const uint8_t group = static_cast<uint8_t>(opcode >> 6);
    const uint8_t y = static_cast<uint8_t>((opcode >> 3) & 0x07u);
    const uint8_t z = static_cast<uint8_t>(opcode & 0x07u);
    const uint8_t value = msx_cpu_get_reg8(state, memory, z);

    if (group == 0) {
        const uint8_t result = msx_cpu_cb_rotate(state, y, value);
        msx_cpu_set_reg8(state, memory, z, result);
        return z == 6 ? 15 : 8;
    }

    if (group == 1) {
        uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | kFlagH);
        if ((value & static_cast<uint8_t>(1u << y)) == 0) {
            flags |= static_cast<uint8_t>(kFlagZ | kFlagPV);
        }
        // BIT only updates S when testing bit 7 and that bit is set.
        if ((y == 7u) && ((value & 0x80u) != 0u)) {
            flags |= kFlagS;
        }
        flags |= static_cast<uint8_t>(value & (kFlagX | kFlagY));
        msx_cpu_set_f(state, flags);
        return z == 6 ? 12 : 8;
    }

    uint8_t result = value;
    if (group == 2) {
        result = static_cast<uint8_t>(value & ~static_cast<uint8_t>(1u << y));
    } else {
        result = static_cast<uint8_t>(value | static_cast<uint8_t>(1u << y));
    }

    msx_cpu_set_reg8(state, memory, z, result);
    return z == 6 ? 15 : 8;
}

void msx_cpu_block_ldi(MsxCpuState* state, MsxMemoryState* memory, int direction)
{
    const uint8_t value = msx_cpu_mem_read8(memory, state->hl);
    msx_cpu_mem_write8(memory, state->de, value);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    state->de = static_cast<uint16_t>(state->de + direction);
    state->bc = static_cast<uint16_t>(state->bc - 1u);

    uint8_t flags = static_cast<uint8_t>(msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagC));
    if (state->bc != 0) {
        flags |= kFlagPV;
    }
    const uint8_t mix = static_cast<uint8_t>(msx_cpu_a(state) + value);
    flags |= static_cast<uint8_t>(mix & (kFlagX | kFlagY));
    msx_cpu_set_f(state, flags);
}

void msx_cpu_block_outi(MsxCpuState* state, MsxMemoryState* memory, int direction)
{
    const uint8_t value = msx_cpu_mem_read8(memory, state->hl);
    msx_memory_out(memory, msx_lo(state->bc), value);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    msx_set_hi(&state->bc, static_cast<uint8_t>(msx_hi(state->bc) - 1u));

    uint8_t flags = static_cast<uint8_t>(kFlagN | (msx_hi(state->bc) & (kFlagS | kFlagX | kFlagY)));
    if (msx_hi(state->bc) == 0) {
        flags |= kFlagZ;
    } else {
        flags |= kFlagPV;
    }
    msx_cpu_set_f(state, flags);
}

// CPI / CPD / CPIR / CPDR: compare A with (HL), HL±=1, BC-=1.
// PV flag reflects BC!=0 after decrement; Z reflects match (A == mem value).
void msx_cpu_block_cpi(MsxCpuState* state, MsxMemoryState* memory, int direction)
{
    const uint8_t mem    = msx_cpu_mem_read8(memory, state->hl);
    const uint8_t a      = msx_cpu_a(state);
    const uint8_t result = static_cast<uint8_t>(a - mem);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    state->bc = static_cast<uint16_t>(state->bc - 1u);

    uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | kFlagN);
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
    msx_cpu_set_f(state, flags);
}

// INI / IND / INIR / INDR: read one byte from port (C) into memory[HL], HL±=1, B-=1.
void msx_cpu_block_ini(MsxCpuState* state, MsxMemoryState* memory, int direction)
{
    const uint8_t value = msx_memory_in(memory, msx_lo(state->bc));
    msx_cpu_mem_write8(memory, state->hl, value);
    state->hl = static_cast<uint16_t>(state->hl + direction);
    msx_set_hi(&state->bc, static_cast<uint8_t>(msx_hi(state->bc) - 1u));

    uint8_t flags = static_cast<uint8_t>(kFlagN | (msx_hi(state->bc) & (kFlagS | kFlagX | kFlagY)));
    if (msx_hi(state->bc) == 0) {
        flags |= kFlagZ;
    } else {
        flags |= kFlagPV;
    }
    msx_cpu_set_f(state, flags);
}

int IRAM_ATTR msx_cpu_step_ed(MsxCpuState* state, MsxMemoryState* memory)
{
    const uint8_t opcode = msx_cpu_fetch8(state, memory);
    const uint8_t regPair = static_cast<uint8_t>((opcode >> 4) & 0x03u);

    switch (opcode) {
        case 0x40:
        case 0x48:
        case 0x50:
        case 0x58:
        case 0x60:
        case 0x68:
        case 0x78: {
            const uint8_t value = msx_memory_in(memory, msx_lo(state->bc));
            msx_cpu_set_reg8(state, memory, static_cast<uint8_t>((opcode >> 3) & 0x07u), value);
            msx_cpu_set_in_flags(state, value);
            return 12;
        }
        case 0x41:
        case 0x49:
        case 0x51:
        case 0x59:
        case 0x61:
        case 0x69:
        case 0x79: {
            const uint8_t value = msx_cpu_get_reg8(state, memory, static_cast<uint8_t>((opcode >> 3) & 0x07u));
            msx_memory_out(memory, msx_lo(state->bc), value);
            return 12;
        }
        case 0x42:
        case 0x52:
        case 0x62:
        case 0x72:
            msx_cpu_sbc16_hl(state, *msx_cpu_reg16_ptr(state, regPair));
            return 15;
        case 0x4A:
        case 0x5A:
        case 0x6A:
        case 0x7A:
            msx_cpu_adc16_hl(state, *msx_cpu_reg16_ptr(state, regPair));
            return 15;
        case 0x43:
        case 0x53:
        case 0x63:
        case 0x73: {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            msx_cpu_mem_write16(memory, address, *msx_cpu_reg16_ptr(state, regPair));
            return 20;
        }
        case 0x4B:
        case 0x5B:
        case 0x6B:
        case 0x7B: {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            *msx_cpu_reg16_ptr(state, regPair) = msx_cpu_mem_read16(memory, address);
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
            msx_cpu_sub8(state, 0, msx_cpu_a(state), 0);
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
            state->pc = msx_cpu_pop16(state, memory);
            state->iff1 = state->iff2;
            if (msx_cpu_trace_pc(from) || msx_cpu_trace_pc(state->pc)) {
                msx_cpu_log_flow("RETN", state, memory, from, state->pc);
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
            state->i = msx_cpu_a(state);
            return 9;
        case 0x4F:
            state->r = msx_cpu_a(state);
            return 9;
        case 0x57: {
            const uint8_t value = state->i;
            msx_cpu_set_a(state, value);
            uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | msx_flags_szxy(value));
            if (state->iff2) {
                flags |= kFlagPV;
            }
            msx_cpu_set_f(state, flags);
            return 9;
        }
        case 0x5F: {
            const uint8_t value = state->r;
            msx_cpu_set_a(state, value);
            uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | msx_flags_szxy(value));
            if (state->iff2) {
                flags |= kFlagPV;
            }
            msx_cpu_set_f(state, flags);
            return 9;
        }
        case 0x70: {
            const uint8_t value = msx_memory_in(memory, msx_lo(state->bc));
            msx_cpu_set_in_flags(state, value);
            return 12;
        }
        case 0x71:
            msx_memory_out(memory, msx_lo(state->bc), 0x00u);
            return 12;
        case 0x77:
        case 0x7F:
            return 8;
        case 0xA0:
            msx_cpu_block_ldi(state, memory, 1);
            return 16;
        case 0xA8:
            msx_cpu_block_ldi(state, memory, -1);
            return 16;
        case 0xB0:
            msx_cpu_block_ldi(state, memory, 1);
            if (state->bc != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xB8:
            msx_cpu_block_ldi(state, memory, -1);
            if (state->bc != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xA3:
            msx_cpu_block_outi(state, memory, 1);
            return 16;
        case 0xAB:
            msx_cpu_block_outi(state, memory, -1);
            return 16;
        case 0xB3:
            msx_cpu_block_outi(state, memory, 1);
            if (msx_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xBB:
            msx_cpu_block_outi(state, memory, -1);
            if (msx_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xA2:                                   // INI
            msx_cpu_block_ini(state, memory, 1);
            return 16;
        case 0xAA:                                   // IND
            msx_cpu_block_ini(state, memory, -1);
            return 16;
        case 0xB2:                                   // INIR
            msx_cpu_block_ini(state, memory, 1);
            if (msx_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xBA:                                   // INDR
            msx_cpu_block_ini(state, memory, -1);
            if (msx_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xA1:                                   // CPI
            msx_cpu_block_cpi(state, memory, 1);
            return 16;
        case 0xA9:                                   // CPD
            msx_cpu_block_cpi(state, memory, -1);
            return 16;
        case 0xB1:                                   // CPIR
            msx_cpu_block_cpi(state, memory, 1);
            if ((msx_cpu_f(state) & kFlagPV) && !(msx_cpu_f(state) & kFlagZ)) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0xB9:                                   // CPDR
            msx_cpu_block_cpi(state, memory, -1);
            if ((msx_cpu_f(state) & kFlagPV) && !(msx_cpu_f(state) & kFlagZ)) {
                state->pc = static_cast<uint16_t>(state->pc - 2u);
                return 21;
            }
            return 16;
        case 0x67: {                                 // RRD
            const uint8_t mem = msx_cpu_mem_read8(memory, state->hl);
            const uint8_t a   = msx_cpu_a(state);
            msx_cpu_mem_write8(memory, state->hl, static_cast<uint8_t>((a << 4) | (mem >> 4)));
            msx_cpu_set_a(state, static_cast<uint8_t>((a & 0xF0u) | (mem & 0x0Fu)));
            msx_cpu_set_f(state, static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | msx_flags_szpxy(msx_cpu_a(state))));
            return 18;
        }
        case 0x6F: {                                 // RLD
            const uint8_t mem = msx_cpu_mem_read8(memory, state->hl);
            const uint8_t a   = msx_cpu_a(state);
            msx_cpu_mem_write8(memory, state->hl, static_cast<uint8_t>((mem << 4) | (a & 0x0Fu)));
            msx_cpu_set_a(state, static_cast<uint8_t>((a & 0xF0u) | (mem >> 4)));
            msx_cpu_set_f(state, static_cast<uint8_t>((msx_cpu_f(state) & kFlagC) | msx_flags_szpxy(msx_cpu_a(state))));
            return 18;
        }
        case 0xFE:
            // BIOS software trap: ED FE is written by msx_disk_apply_rom_patches()
            // at the DISK ROM entry points (PHYDIO, DSKCHG, GETDPB, DSKFMT, DRVOFF).
            if (memory->diskPatch) {
                memory->diskPatch(state, memory, state->lastPc);
            }
            return 8;
        default:
            // Unassigned ED-prefixed opcodes behave as NOPs on the Z80.
            return 8;
    }
}

int IRAM_ATTR msx_cpu_step_opcode(MsxCpuState* state, MsxMemoryState* memory)
{
    if (MSX_CPU_UNLIKELY(!state || !memory)) {
        return 0;
    }

    state->lastPc = state->pc;
    msx_cpu_log_context(state, memory, state->lastPc);
    const uint8_t opcode = msx_cpu_fetch8(state, memory);
    msx_cpu_log_opcode(state, memory, state->lastPc, opcode);

    state->lastOpcode = opcode;

    if ((opcode & 0xC7u) == 0x04u) {
        const uint8_t reg = static_cast<uint8_t>((opcode >> 3) & 0x07u);
        const uint8_t value = msx_cpu_get_reg8(state, memory, reg);
        msx_cpu_set_reg8(state, memory, reg, msx_cpu_inc8(state, value));
        return reg == 6 ? 11 : 4;
    }

    if ((opcode & 0xC7u) == 0x05u) {
        const uint8_t reg = static_cast<uint8_t>((opcode >> 3) & 0x07u);
        const uint8_t value = msx_cpu_get_reg8(state, memory, reg);
        msx_cpu_set_reg8(state, memory, reg, msx_cpu_dec8(state, value));
        return reg == 6 ? 11 : 4;
    }

    if ((opcode & 0xC7u) == 0x06u) {
        const uint8_t reg = static_cast<uint8_t>((opcode >> 3) & 0x07u);
        const uint8_t value = msx_cpu_fetch8(state, memory);
        msx_cpu_set_reg8(state, memory, reg, value);
        return reg == 6 ? 10 : 7;
    }

    if ((opcode & 0xCFu) == 0x01u) {
        *msx_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)) = msx_cpu_fetch16(state, memory);
        return 10;
    }

    if ((opcode & 0xCFu) == 0x03u) {
        uint16_t* reg = msx_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u));
        *reg = static_cast<uint16_t>(*reg + 1u);
        return 6;
    }

    if ((opcode & 0xCFu) == 0x0Bu) {
        uint16_t* reg = msx_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u));
        *reg = static_cast<uint16_t>(*reg - 1u);
        return 6;
    }

    if ((opcode & 0xCFu) == 0x09u) {
        msx_cpu_add16_hl(state, *msx_cpu_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)));
        return 11;
    }

    if ((opcode & 0xC0u) == 0x40u) {
        if (opcode == 0x76u) {
            state->halted = true;
            state->runState = MsxCpuRunState::Halted;
            #if MSX_CPU_TRACE_ENABLED
            static uint16_t s_haltEnterLogCount = 0u;
            if (s_haltEnterLogCount < 8u) {
                const uint8_t next0 = msx_memory_read8(memory, state->lastPc);
                const uint8_t next1 = msx_memory_read8(memory, static_cast<uint16_t>(state->lastPc + 1u));
                const uint8_t next2 = msx_memory_read8(memory, static_cast<uint16_t>(state->lastPc + 2u));
                const uint8_t next3 = msx_memory_read8(memory, static_cast<uint16_t>(state->lastPc + 3u));
                const uint8_t hook0 = msx_memory_read8(memory, 0xFD9Au);
                const uint8_t hook1 = msx_memory_read8(memory, 0xFD9Bu);
                const uint8_t hook2 = msx_memory_read8(memory, 0xFD9Cu);
                std::printf("[MSX][HALT] enter at=%04X next=%04X iff1=%u irq=%u A8=%02X SSL3=%02X cart=%s banks=%u/%u/%u/%u bytes=%02X %02X %02X %02X hook=%02X %02X %02X #%u\n",
                            static_cast<unsigned>(state->lastPc),
                            static_cast<unsigned>(state->pc),
                            state->iff1 ? 1u : 0u,
                            state->irqPending ? 1u : 0u,
                            static_cast<unsigned>(memory->slotRegister),
                            static_cast<unsigned>(memory->secondarySlotRegs[3]),
                            msx_media_cartridge_type_label(memory->cart.type),
                            static_cast<unsigned>(memory->cart.windowBanks[0]),
                            static_cast<unsigned>(memory->cart.windowBanks[1]),
                            static_cast<unsigned>(memory->cart.windowBanks[2]),
                            static_cast<unsigned>(memory->cart.windowBanks[3]),
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
        const uint8_t value = msx_cpu_get_reg8(state, memory, src);
        msx_cpu_set_reg8(state, memory, dst, value);
        return (dst == 6 || src == 6) ? 7 : 4;
    }

    if ((opcode & 0xC0u) == 0x80u) {
        const uint8_t value = msx_cpu_get_reg8(state, memory, static_cast<uint8_t>(opcode & 0x07u));
        msx_cpu_do_alu(state, static_cast<uint8_t>((opcode >> 3) & 0x07u), value);
        return (opcode & 0x07u) == 6 ? 7 : 4;
    }

    if ((opcode & 0xC7u) == 0xC0u) {
        if (msx_cpu_condition(state, static_cast<uint8_t>((opcode >> 3) & 0x07u))) {
            const uint16_t from = state->lastPc;
            state->pc = msx_cpu_pop16(state, memory);
            if (msx_cpu_trace_pc(from) || msx_cpu_trace_pc(state->pc)) {
                msx_cpu_log_flow("RETcc", state, memory, from, state->pc);
            }
            return 11;
        }
        return 5;
    }

    if ((opcode & 0xC7u) == 0xC2u) {
        const uint16_t address = msx_cpu_fetch16(state, memory);
        if (msx_cpu_condition(state, static_cast<uint8_t>((opcode >> 3) & 0x07u))) {
            msx_cpu_restore_cart_boot_mapping(memory, address, state->lastPc);
            msx_cpu_log_bios_entry("JPcc", state, memory, state->lastPc, address, state->pc);
            if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(address)) {
                msx_cpu_log_flow("JPcc", state, memory, state->lastPc, address);
            }
            state->pc = address;
        }
        return 10;
    }

    if ((opcode & 0xC7u) == 0xC4u) {
        const uint16_t address = msx_cpu_fetch16(state, memory);
        if (msx_cpu_condition(state, static_cast<uint8_t>((opcode >> 3) & 0x07u))) {
            msx_cpu_log_bios_entry("CALLcc", state, memory, state->lastPc, address, state->pc);
            msx_cpu_push16(state, memory, state->pc);
            if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(address)) {
                msx_cpu_log_flow("CALLcc", state, memory, state->lastPc, address);
            }
            state->pc = address;
            return 17;
        }
        return 10;
    }

    if ((opcode & 0xCFu) == 0xC1u) {
        *msx_cpu_stack_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)) = msx_cpu_pop16(state, memory);
        return 10;
    }

    if ((opcode & 0xCFu) == 0xC5u) {
        msx_cpu_push16(state, memory, *msx_cpu_stack_reg16_ptr(state, static_cast<uint8_t>((opcode >> 4) & 0x03u)));
        return 11;
    }

    if ((opcode & 0xC7u) == 0xC7u) {
        msx_cpu_push16(state, memory, state->pc);
        state->pc = static_cast<uint16_t>(opcode & 0x38u);
        if (state->pc == 0x0030u) {
            msx_cpu_log_rst30(state, memory, state->lastPc, state->sp ? msx_cpu_mem_read16(memory, state->sp) : 0u);
        }
        msx_cpu_log_bios_entry("RST", state, memory, state->lastPc, state->pc, msx_cpu_mem_read16(memory, state->sp));
        if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc)) {
            msx_cpu_log_flow("RST", state, memory, state->lastPc, state->pc);
        }
        return 11;
    }

    switch (opcode) {
        case 0x00:
            return 4;
        case 0x02:
            msx_cpu_mem_write8(memory, state->bc, msx_cpu_a(state));
            return 7;
        case 0x07:
            msx_cpu_acc_rotate_left_carry(state);
            return 4;
        case 0x08:
            msx_cpu_exchange16(&state->af, &state->af2);
            return 4;
        case 0x0A:
            msx_cpu_set_a(state, msx_cpu_mem_read8(memory, state->bc));
            return 7;
        case 0x0F:
            msx_cpu_acc_rotate_right_carry(state);
            return 4;
        case 0x10: {
            const int8_t offset = msx_signed_offset(msx_cpu_fetch8(state, memory));
            msx_set_hi(&state->bc, static_cast<uint8_t>(msx_hi(state->bc) - 1u));
            if (msx_hi(state->bc) != 0) {
                state->pc = static_cast<uint16_t>(state->pc + offset);
                if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc)) {
                    msx_cpu_log_flow("DJNZ", state, memory, state->lastPc, state->pc);
                }
                return 13;
            }
            return 8;
        }
        case 0x12:
            msx_cpu_mem_write8(memory, state->de, msx_cpu_a(state));
            return 7;
        case 0x17:
            msx_cpu_acc_rotate_left(state);
            return 4;
        case 0x18: {
            const int8_t offset = msx_signed_offset(msx_cpu_fetch8(state, memory));
            state->pc = static_cast<uint16_t>(state->pc + offset);
            if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc)) {
                msx_cpu_log_flow("JR", state, memory, state->lastPc, state->pc);
            }
            return 12;
        }
        case 0x1A:
            msx_cpu_set_a(state, msx_cpu_mem_read8(memory, state->de));
            return 7;
        case 0x1F:
            msx_cpu_acc_rotate_right(state);
            return 4;
        case 0x20:
        case 0x28:
        case 0x30:
        case 0x38: {
            const int8_t offset = msx_signed_offset(msx_cpu_fetch8(state, memory));
            const uint8_t condition = static_cast<uint8_t>((opcode >> 3) & 0x03u);
            if (msx_cpu_condition(state, condition)) {
                state->pc = static_cast<uint16_t>(state->pc + offset);
                if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc)) {
                    msx_cpu_log_flow("JRcc", state, memory, state->lastPc, state->pc);
                }
                return 12;
            }
            return 7;
        }
        case 0x22: {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            msx_cpu_mem_write16(memory, address, state->hl);
            return 16;
        }
        case 0x27:
            msx_cpu_daa(state);
            return 4;
        case 0x2A: {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            state->hl = msx_cpu_mem_read16(memory, address);
            return 16;
        }
        case 0x2F:
            msx_cpu_set_a(state, static_cast<uint8_t>(msx_cpu_a(state) ^ 0xFFu));
            msx_cpu_set_f(state, static_cast<uint8_t>((msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV | kFlagC)) |
                                                      (msx_cpu_a(state) & (kFlagX | kFlagY)) |
                                                      kFlagH | kFlagN));
            return 4;
        case 0x32: {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            msx_cpu_mem_write8(memory, address, msx_cpu_a(state));
            return 13;
        }
        case 0x37:
            msx_cpu_set_f(state, static_cast<uint8_t>((msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) |
                                                      (msx_cpu_a(state) & (kFlagX | kFlagY)) |
                                                      kFlagC));
            return 4;
        case 0x3A: {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            msx_cpu_set_a(state, msx_cpu_mem_read8(memory, address));
            return 13;
        }
        case 0x3F: {
            const bool oldCarry = (msx_cpu_f(state) & kFlagC) != 0;
            uint8_t flags = static_cast<uint8_t>((msx_cpu_f(state) & (kFlagS | kFlagZ | kFlagPV)) |
                                                 (msx_cpu_a(state) & (kFlagX | kFlagY)));
            if (oldCarry) {
                flags |= kFlagH;
            } else {
                flags |= kFlagC;
            }
            msx_cpu_set_f(state, flags);
            return 4;
        }
        case 0xC3:
        {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            msx_cpu_restore_cart_boot_mapping(memory, address, state->lastPc);
            msx_cpu_log_bios_entry("JP", state, memory, state->lastPc, address, state->pc);
            state->pc = address;
            if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc)) {
                msx_cpu_log_flow("JP", state, memory, state->lastPc, state->pc);
            }
            return 10;
        }
        case 0xC6:
            msx_cpu_do_alu(state, 0, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xC9:
            state->pc = msx_cpu_pop16(state, memory);
            if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->pc)) {
                msx_cpu_log_flow("RET", state, memory, state->lastPc, state->pc);
            }
            return 10;
        case 0xCB:
            return msx_cpu_step_cb(state, memory);
        case 0xCD: {
            const uint16_t address = msx_cpu_fetch16(state, memory);
            msx_cpu_log_bios_entry("CALL", state, memory, state->lastPc, address, state->pc);
            msx_cpu_push16(state, memory, state->pc);
            if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(address)) {
                msx_cpu_log_flow("CALL", state, memory, state->lastPc, address);
            }
            state->pc = address;
            return 17;
        }
        case 0xCE:
            msx_cpu_do_alu(state, 1, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xD3:
            msx_memory_out(memory, msx_cpu_fetch8(state, memory), msx_cpu_a(state));
            return 11;
        case 0xD6:
            msx_cpu_do_alu(state, 2, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xD9:
            msx_cpu_exchange16(&state->bc, &state->bc2);
            msx_cpu_exchange16(&state->de, &state->de2);
            msx_cpu_exchange16(&state->hl, &state->hl2);
            return 4;
        case 0xDB:
            // IN A,(n): per Z80 spec, flags are NOT affected.
            msx_cpu_set_a(state, msx_memory_in(memory, msx_cpu_fetch8(state, memory)));
            return 11;
        case 0xDE:
            msx_cpu_do_alu(state, 3, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xE3: {
            const uint16_t memoryValue = msx_cpu_mem_read16(memory, state->sp);
            msx_cpu_mem_write16(memory, state->sp, state->hl);
            state->hl = memoryValue;
            return 19;
        }
        case 0xE6:
            msx_cpu_do_alu(state, 4, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xE9:
            msx_cpu_restore_cart_boot_mapping(memory, state->hl, state->lastPc);
            msx_cpu_log_bios_entry("JP(HL)", state, memory, state->lastPc, state->hl, state->pc);
            if (msx_cpu_trace_pc(state->lastPc) || msx_cpu_trace_pc(state->hl)) {
                msx_cpu_log_flow("JP(HL)", state, memory, state->lastPc, state->hl);
            }
            state->pc = state->hl;
            return 4;
        case 0xEB:
            msx_cpu_exchange16(&state->de, &state->hl);
            return 4;
        case 0xED:
            return msx_cpu_step_ed(state, memory);
        case 0xEE:
            msx_cpu_do_alu(state, 5, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xF3:
            state->iff1    = false;
            state->iff2    = false;
            state->eiDelay = 0u;
            return 4;
        case 0xF6:
            msx_cpu_do_alu(state, 6, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xF9:
            state->sp = state->hl;
            return 6;
        case 0xFB:
            // EI: enable interrupts after the *next* instruction (Z80 one-instruction delay).
            state->eiDelay = 2u;
            return 4;
        case 0xFE:
            msx_cpu_do_alu(state, 7, msx_cpu_fetch8(state, memory));
            return 7;
        case 0xDD: return msx_cpu_step_xy(state, memory, &state->ix);
        case 0xFD: return msx_cpu_step_xy(state, memory, &state->iy);
        default:
            msx_cpu_mark_unsupported(state, opcode);
            return 0;
    }
}

} // namespace

void msx_cpu_init(MsxCpuState* state)
{
    if (!state) {
        return;
    }

    msx_cpu_clear_pending_psg();
    std::memset(state, 0, sizeof(*state));
    state->ix = 0xFFFFu;
    state->iy = 0xFFFFu;
    msx_cpu_invalidate_fetch_ptr(state);
    state->runState = MsxCpuRunState::Running;
}

void msx_cpu_reset(MsxCpuState* state, uint16_t resetPc, uint16_t resetSp)
{
    if (!state) {
        return;
    }

    msx_cpu_clear_pending_psg();
    std::memset(state, 0, sizeof(*state));
    state->pc = resetPc;
    state->sp = resetSp;
    state->ix = 0xFFFFu;
    state->iy = 0xFFFFu;
    state->af = 0x0040u;
    msx_cpu_invalidate_fetch_ptr(state);
    state->runState = MsxCpuRunState::Running;
}

void msx_cpu_request_irq(MsxCpuState* state)
{
    if (!state) {
        return;
    }

#if MSX_CPU_TRACE_ENABLED
    static uint16_t s_irqRequestLogCount = 0u;
    if (s_irqRequestLogCount < 64u &&
        (msx_cpu_trace_pc(state->pc) || state->halted)) {
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

void IRAM_ATTR msx_cpu_flush_pending_psg(MsxMemoryState* memory)
{
    msx_cpu_flush_pending_psg_impl(memory);
}

void msx_cpu_clear_pending_psg()
{
    s_pendingPsgCycles = 0u;
}

int IRAM_ATTR msx_cpu_run_cycles(MsxCpuState* state, MsxMemoryState* memory, int cycleBudget)
{
    if (!state || !memory || cycleBudget <= 0) {
        return 0;
    }

    if (state->runState == MsxCpuRunState::Unsupported || state->runState == MsxCpuRunState::Faulted) {
        return 0;
    }

    int usedCycles = 0;
    while (usedCycles < cycleBudget) {
        if (state->irqPending && state->iff1) {
            const int irqCycles = msx_cpu_service_irq(state, memory);
            usedCycles += irqCycles;
            state->totalCycles += static_cast<uint32_t>(irqCycles);
            if (irqCycles > 0) {
                msx_cpu_accumulate_psg_cycles(memory, static_cast<uint32_t>(irqCycles));
            }
            continue;
        }

        if (state->halted) {
            state->runState = MsxCpuRunState::Halted;
            const int burn = cycleBudget - usedCycles;
            usedCycles += burn;
            state->totalCycles += static_cast<uint32_t>(burn);
            if (burn > 0) {
                msx_cpu_accumulate_psg_cycles(memory, static_cast<uint32_t>(burn));
            }
            break;
        }

        state->runState = MsxCpuRunState::Running;
        const int stepCycles = msx_cpu_step_opcode(state, memory);
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
        msx_cpu_accumulate_psg_cycles(memory, static_cast<uint32_t>(stepCycles));
    }

    return usedCycles;
}

const char* msx_cpu_run_state_label(MsxCpuRunState state)
{
    switch (state) {
        case MsxCpuRunState::Running:
            return "RUN";
        case MsxCpuRunState::Halted:
            return "HALT";
        case MsxCpuRunState::Unsupported:
            return "UNSUP";
        case MsxCpuRunState::Faulted:
        default:
            return "FAULT";
    }
}

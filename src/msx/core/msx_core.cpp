#include "msx_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <SD.h>
#include <esp_heap_caps.h>

#include "msx_disk.h"
#include "../msx_video.h"

#ifndef MSX_CORE_LOG_ENABLED
#define MSX_CORE_LOG_ENABLED 1
#endif

#ifndef MSX_CORE_TRACE_ENABLED
#define MSX_CORE_TRACE_ENABLED 0
#endif

#if MSX_CORE_LOG_ENABLED
#define MSX_CORE_LOG(...) std::printf(__VA_ARGS__)
#else
#define MSX_CORE_LOG(...) do { } while (0)
#endif

namespace {

constexpr int kMsxFrameCycles60Hz = 59659;
// BIOS-only boot starts with all pages on primary slot 0.
// Cartridge-assisted boot needs the cart visible at 4000h-BFFFh while page 0
// stays on BIOS and page 3 stays on expanded RAM.
constexpr uint8_t kMsxBootSlotBios = 0x00;
constexpr uint8_t kMsxBootSlotCart = 0xD4;
constexpr uint8_t kMsxBootSecondaryBios = 0x00;
constexpr uint8_t kMsxBootSecondaryCart = 0xA0;
// Disk-sector boot needs DiskROM visible in page1 and RAM in pages 2-3.
constexpr uint8_t kMsxBootSlotDisk = 0xFC;
constexpr uint8_t kMsxBootSecondaryDisk = 0xA4;
constexpr uint16_t kMsxDefaultStack = 0xF380;
constexpr uint16_t kMsxDiskBootAddress = 0xC000;

uint8_t msx_core_default_secondary_slot_reg(MsxMachineMode machineMode, bool directBoot)
{
    if (machineMode == MsxMachineMode::MSX2) {
        return kMsxBootSecondaryDisk;
    }
    return directBoot ? kMsxBootSecondaryCart : kMsxBootSecondaryBios;
}
constexpr uint32_t kMsxStatusRefreshPeriod = 8u;
constexpr size_t kMsxCartRamSizeMsx1 = 0x8000u;
constexpr uint8_t kMsxSlotIdMainRam = 0x8Bu;   // expanded slot 3-2
constexpr uint8_t kMsxSlotIdDiskRom = 0x87u;   // expanded slot 3-1
constexpr uint8_t kMsxSlotIdCartridge = 0x01u; // primary slot 1
constexpr uint8_t kMsxInputCfgJoy = 0x01u;
constexpr uint8_t kMsxInputCfgKeyboard = 0x02u;
constexpr uint8_t kMsxInputCfgVaus = 0x04u;
constexpr uint16_t kMsxAddrExptbl = 0xFCC1u;
constexpr uint16_t kMsxAddrSlttbl = 0xFCC5u;
constexpr uint16_t kMsxAddrSltatr = 0xFCCCu;  // slot attribute table (60 bytes)
constexpr uint16_t kMsxAddrSltwrk = 0xFD09u;  // slot work area (128 bytes)
constexpr uint16_t kMsxAddrDrvInv = 0xFB21u;
constexpr uint16_t kMsxAddrRamAd0 = 0xF341u;
constexpr uint16_t kMsxAddrMaster = 0xF348u;
constexpr uint16_t kMsxAddrCartInitLo = 0xF7C5u;
constexpr uint16_t kMsxAddrCartInitHi = 0xF7C6u;
constexpr uint16_t kMsxAddrCartInitSlot = 0xF7C7u;
constexpr size_t kMsxRamSizeMsx2 = 0x20000u;
constexpr size_t kMsxRamSizeMsx1 = 0x10000u;
constexpr size_t kMsxPageSize16K = 0x4000u;
constexpr size_t kMsx2VramSize = 0x20000u;
constexpr size_t kMsxCoreInitReserve = 0x4000u;

size_t msx_core_select_msx2_ram_size()
{
    size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t minRam = kMsxRamSizeMsx1;
    if (freeInternal <= (kMsx2VramSize + kMsxCoreInitReserve)) {
        return minRam;
    }

    size_t budget = freeInternal - (kMsx2VramSize + kMsxCoreInitReserve);
    if (budget < minRam) {
        return minRam;
    }

    budget &= ~(kMsxPageSize16K - 1u);
    if (budget < minRam) {
        return minRam;
    }

    return (budget > kMsxRamSizeMsx2) ? kMsxRamSizeMsx2 : budget;
}

void msx_core_set_status(MsxCoreState* state, const char* format, ...)
{
    if (!state || !format) {
        return;
    }

    va_list args;
    va_start(args, format);
    std::vsnprintf(state->statusText, sizeof(state->statusText), format, args);
    va_end(args);
}

void msx_core_capture_status_state(MsxCoreState* state)
{
    if (!state) {
        return;
    }

    state->lastStatusFrame = state->frameCounter;
    state->lastStatusMode = static_cast<uint8_t>(state->vdp.mode);
    state->lastStatusRunState = static_cast<uint8_t>(state->cpu.runState);
    state->lastStatusUnsupportedOpcode = state->cpu.unsupportedOpcode;
    state->lastStatusPc = (state->cpu.runState == MsxCpuRunState::Unsupported)
                              ? state->cpu.unsupportedPc
                              : state->cpu.pc;
}

bool msx_core_status_needs_refresh(const MsxCoreState* state)
{
    if (!state) {
        return false;
    }

    if (state->frameCounter == 0u) {
        return true;
    }

    if (state->lastStatusRunState != static_cast<uint8_t>(state->cpu.runState)) {
        return true;
    }

    if (state->lastStatusMode != static_cast<uint8_t>(state->vdp.mode)) {
        return true;
    }

    if (state->cpu.runState == MsxCpuRunState::Unsupported) {
        return state->lastStatusUnsupportedOpcode != state->cpu.unsupportedOpcode ||
               state->lastStatusPc != state->cpu.unsupportedPc;
    }

    return (state->frameCounter - state->lastStatusFrame) >= kMsxStatusRefreshPeriod;
}

void msx_core_refresh_status(MsxCoreState* state)
{
    if (!state) {
        return;
    }

    switch (state->cpu.runState) {
        case MsxCpuRunState::Running:
        case MsxCpuRunState::Halted:
            msx_core_set_status(state,
                                "%s %s PC %04X %s",
                                msx_media_bios_target_label(state->biosTarget),
                                msx_cpu_run_state_label(state->cpu.runState),
                                state->cpu.pc,
                                msx_vdp_mode_label(state->vdp.mode));
            break;
        case MsxCpuRunState::Unsupported:
            msx_core_set_status(state,
                                "%s UNSUP %02X %04X",
                                msx_media_bios_target_label(state->biosTarget),
                                state->cpu.unsupportedOpcode,
                                state->cpu.unsupportedPc);
            break;
        case MsxCpuRunState::Faulted:
        default:
            msx_core_set_status(state,
                                "%s FAULT %04X",
                                msx_media_bios_target_label(state->biosTarget),
                                state->cpu.pc);
            break;
    }

    msx_core_capture_status_state(state);
}

uint16_t msx_core_select_boot_pc(const MsxCartState* cart, bool* directBoot)
{
    if (directBoot) {
        *directBoot = false;
    }

    if (!cart || !cart->directBootCandidate) {
        return 0x0000u;
    }

    // Standard "AB" cartridge headers should boot through the BIOS with the
    // cartridge visible at 4000h-BFFFh. Jumping directly into the header
    // routine skips BIOS machine setup and breaks games such as Bomberman.
    if (directBoot) {
        *directBoot = true;
    }
    return 0x0000u;
}

void msx_core_attach_runtime_devices(MsxCoreState* state)
{
    if (!state) {
        return;
    }

    state->memory.diskPatch = msx_disk_bios_patch_handler;
    msx_memory_attach_vdp(&state->memory, &state->vdp);
    if (state->audioHookReady) {
        msx_memory_attach_psg(&state->memory, &state->psg);
        msx_psg_reset(&state->psg);
    } else {
        msx_memory_attach_psg(&state->memory, nullptr);
    }
}

uint8_t msx_core_ram_segment_for_page(const MsxMemoryState* memory, uint8_t pageIndex)
{
    if (!memory || memory->ramSegmentCount == 0u) {
        return 0u;
    }

    if (memory->mapperEnabled && memory->ramSegmentCount > 4u) {
        return static_cast<uint8_t>(memory->mapperRegisters[pageIndex & 0x03u] % memory->ramSegmentCount);
    }

    return static_cast<uint8_t>(pageIndex % memory->ramSegmentCount);
}

uint8_t* msx_core_raw_ram_bank_ptr(MsxMemoryState* memory, uint8_t pageIndex, uint8_t subPage)
{
    if (!memory || (pageIndex >= 4u) || (subPage >= 2u) || (memory->ramSegmentCount == 0u)) {
        return nullptr;
    }

    const uint8_t segment = msx_core_ram_segment_for_page(memory, pageIndex);
    const uint8_t bankIndex = static_cast<uint8_t>(segment * 2u + subPage);
    if ((segment >= memory->ramSegmentCount) ||
        (bankIndex >= memory->ramBankCount) ||
        (bankIndex >= 16u)) {
        return nullptr;
    }

    return memory->ramBanks[bankIndex];
}

void msx_core_raw_page3_write8(MsxMemoryState* memory, uint16_t address, uint8_t value)
{
    if (!memory || !memory->ready || ((address >> 14) != 3u)) {
        return;
    }

    uint8_t* const bankPtr = msx_core_raw_ram_bank_ptr(
        memory,
        3u,
        static_cast<uint8_t>((address >> 13) & 0x01u)
    );
    if (!bankPtr) {
        return;
    }

    bankPtr[address & 0x1FFFu] = value;
}

void msx_core_seed_slot_work_area(MsxCoreState* state, uint8_t secondarySlotReg)
{
    if (!state || !state->memory.ready) {
        return;
    }

    MsxMemoryState* const memory = &state->memory;
    const bool hasDiskRom = (memory->diskRom != nullptr) && (memory->diskRomSize != 0u);
    const bool hasCartInit = memory->cart.ready &&
                             memory->cart.directBootCandidate &&
                             (memory->cart.initAddress >= 0x4000u) &&
                             (memory->cart.initAddress < 0xC000u);

    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 0u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 1u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 2u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 3u), 0x80u);

    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 0u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 1u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 2u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 3u), secondarySlotReg);

    for (uint16_t i = 0u; i < 4u; ++i) {
        msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrRamAd0 + i), kMsxSlotIdMainRam);
    }

    msx_core_raw_page3_write8(memory, kMsxAddrMaster, hasDiskRom ? kMsxSlotIdDiskRom : 0x00u);

    for (uint16_t i = 0u; i < 4u; ++i) {
        const bool primaryInterface = hasDiskRom && (i == 0u);
        msx_core_raw_page3_write8(memory,
                                  static_cast<uint16_t>(kMsxAddrDrvInv + (i * 2u)),
                                  primaryInterface ? kMsxSlotIdDiskRom : 0x00u);
        msx_core_raw_page3_write8(memory,
                                  static_cast<uint16_t>(kMsxAddrDrvInv + (i * 2u) + 1u),
                                  primaryInterface ? 0x01u : 0x00u);
    }

    // Zero SLTATR and SLTWRK so heap garbage doesn't cause the BIOS extension ROM
    // scan to misread slot attributes and issue spurious CALLFs into empty slots.
    for (uint16_t i = 0u; i < 60u; ++i) {
        msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSltatr + i), 0x00u);
    }
    for (uint16_t i = 0u; i < 128u; ++i) {
        msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSltwrk + i), 0x00u);
    }

    if (hasCartInit) {
        msx_core_raw_page3_write8(memory,
                                  kMsxAddrCartInitLo,
                                  static_cast<uint8_t>(memory->cart.initAddress & 0x00FFu));
        msx_core_raw_page3_write8(memory,
                                  kMsxAddrCartInitHi,
                                  static_cast<uint8_t>(memory->cart.initAddress >> 8));
        msx_core_raw_page3_write8(memory, kMsxAddrCartInitSlot, kMsxSlotIdCartridge);
    } else {
        msx_core_raw_page3_write8(memory, kMsxAddrCartInitSlot, 0x00u);
    }
    memory->cartBootWorkareaFallbackArmed = hasCartInit;
    memory->cartBootMappingRestoreArmed = hasCartInit;

    MSX_CORE_LOG("[MSX] slot workarea: EXPTBL=%02X/%02X/%02X/%02X SLTTBL=%02X/%02X/%02X/%02X RAMAD=%02X MASTER=%02X CART=%02X INIT=%04X SLOT=%02X\n",
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x80u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(secondarySlotReg),
                 static_cast<unsigned>(kMsxSlotIdMainRam),
                 static_cast<unsigned>(hasDiskRom ? kMsxSlotIdDiskRom : 0x00u),
                 static_cast<unsigned>(hasCartInit ? kMsxSlotIdCartridge : 0x00u),
                 static_cast<unsigned>(hasCartInit ? memory->cart.initAddress : 0x0000u),
                 static_cast<unsigned>(hasCartInit ? kMsxSlotIdCartridge : 0x00u));
}

void msx_core_apply_boot_mapping(MsxCoreState* state,
                                 uint8_t slotRegister,
                                 uint8_t secondarySlotReg)
{
    if (!state) {
        return;
    }

    state->memory.slotRegister = slotRegister;
    std::memset(state->memory.secondarySlotRegs, 0, sizeof(state->memory.secondarySlotRegs));
    state->memory.secondarySlotRegs[3] = secondarySlotReg;
    state->memory.lastPortA8 = slotRegister;
    msx_memory_refresh_maps(&state->memory);
    msx_core_seed_slot_work_area(state, secondarySlotReg);

    MSX_CORE_LOG("[MSX] boot map: A8=%02X FFFF=%02X\n",
                 static_cast<unsigned>(slotRegister),
                 static_cast<unsigned>(secondarySlotReg));
}

void msx_core_finish_no_cart_init(MsxCoreState* state)
{
    msx_core_attach_runtime_devices(state);
    msx_core_apply_boot_mapping(state,
                                kMsxBootSlotBios,
                                msx_core_default_secondary_slot_reg(state->machineMode, false));

    state->bootPc = 0x0000u;
    state->directBoot = false;
    msx_cpu_init(&state->cpu);
    msx_cpu_reset(&state->cpu, state->bootPc, kMsxDefaultStack);

    msx_vdp_render(&state->vdp);
    msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);

    state->initialized = true;
    msx_core_capture_status_state(state);
}

bool msx_core_try_boot_disk_sector(MsxCoreState* state)
{
    if (!state || !state->memory.diskRom || !state->memory.disk || !state->memory.disk->dskData) {
        return false;
    }

    uint8_t sector[kMsxDskSectorSize] = {};
    if (!msx_disk_read_logical_sector(state->memory.disk, 0u, sector)) {
        std::printf("[MSX] disk boot: sector 0 read failed\n");
        return false;
    }

    const uint16_t bytesPerSector =
        static_cast<uint16_t>(sector[0x0Bu] | (static_cast<uint16_t>(sector[0x0Cu]) << 8));
    const uint16_t totalSectors =
        static_cast<uint16_t>(sector[0x13u] | (static_cast<uint16_t>(sector[0x14u]) << 8));
    const uint16_t sectorsPerFat =
        static_cast<uint16_t>(sector[0x16u] | (static_cast<uint16_t>(sector[0x17u]) << 8));
    const uint16_t signature =
        static_cast<uint16_t>(sector[0x1FEu] | (static_cast<uint16_t>(sector[0x1FFu]) << 8));
    std::printf("[MSX] disk boot: jump=%02X %02X %02X bps=%u spc=%u media=%02X total=%u spf=%u sig=%04X\n",
                static_cast<unsigned>(sector[0x00u]),
                static_cast<unsigned>(sector[0x01u]),
                static_cast<unsigned>(sector[0x02u]),
                static_cast<unsigned>(bytesPerSector),
                static_cast<unsigned>(sector[0x0Du]),
                static_cast<unsigned>(sector[0x15u]),
                static_cast<unsigned>(totalSectors),
                static_cast<unsigned>(sectorsPerFat),
                static_cast<unsigned>(signature));

    const bool placeholderLoop =
        (sector[0x00u] == 0xEBu) &&
        (sector[0x01u] == 0xFEu);
    if (placeholderLoop) {
        std::printf("[MSX] disk boot: sector 0 is a placeholder loop (%02X %02X %02X), using BIOS/DISK ROM path\n",
                    static_cast<unsigned>(sector[0x00u]),
                    static_cast<unsigned>(sector[0x01u]),
                    static_cast<unsigned>(sector[0x02u]));
        return false;
    }

    const uint8_t jump = sector[0];
    if (jump != 0xEBu && jump != 0xE9u && jump != 0xC3u) {
        std::printf("[MSX] disk boot: sector 0 is not executable (opcode=%02X bytes=%02X %02X %02X)\n",
                    static_cast<unsigned>(jump),
                    static_cast<unsigned>(sector[0x00u]),
                    static_cast<unsigned>(sector[0x01u]),
                    static_cast<unsigned>(sector[0x02u]));
        return false;
    }

    msx_core_apply_boot_mapping(state, kMsxBootSlotDisk, kMsxBootSecondaryDisk);

    for (uint16_t i = 0; i < static_cast<uint16_t>(kMsxDskSectorSize); ++i) {
        msx_memory_write8(&state->memory, static_cast<uint16_t>(kMsxDiskBootAddress + i), sector[i]);
    }

    state->bootPc = kMsxDiskBootAddress;
    state->directBoot = true;
    msx_cpu_reset(&state->cpu, state->bootPc, kMsxDefaultStack);
    state->cpu.af = 0x0000u; // A=0 selects drive A: for the boot sector entry path.

    msx_vdp_render(&state->vdp);
    msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);

    std::printf("[MSX] disk boot: sector 0 loaded at %04X, slot=%02X\n",
                static_cast<unsigned>(state->bootPc),
                static_cast<unsigned>(state->memory.slotRegister));
    return true;
}

void msx_core_init_audio(MsxCoreState* state, uint32_t audioSampleRate)
{
    if (!state) {
        return;
    }

    state->audioSampleRate = audioSampleRate;
    state->audioHookReady = false;

    if (audioSampleRate == 0u) {
        return;
    }

    if (msx_psg_init(&state->psg, audioSampleRate)) {
        state->audioHookReady = true;
        return;
    }

    std::printf("[MSX] core audio: PSG init failed, continuing without sound\n");
}

} // namespace

bool msx_core_init(MsxCoreState* state,
                   const MsxRomImage* rom,
                   const MsxBiosBundle* bios,
                   const char* romName,
                   uint32_t audioSampleRate)
{
    if (!state || !rom || !bios || !rom->data || rom->size == 0 || !rom->sizeSupported || !bios->compatible) {
        std::printf("[MSX] core init failed: invalid launch data\n");
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->rom = *rom;
    state->biosTarget = bios->target;
    state->machineMode = msx_media_target_to_machine_mode(bios->target);
    state->videoHookReady = true;

    std::snprintf(state->romName,
                  sizeof(state->romName),
                  "%s",
                  (romName && romName[0] != '\0') ? romName : "Unknown.rom");

    std::printf("[MSX] core init: bios begin\n");
    if (!msx_bios_init(&state->bios, bios)) {
        std::printf("[MSX] core init failed at bios init\n");
        return false;
    }

    std::printf("[MSX] core init: cart begin\n");
    if (!msx_cart_init(&state->cart, rom)) {
        std::printf("[MSX] core init failed at cart init\n");
        msx_bios_shutdown(&state->bios);
        return false;
    }

    std::printf("[MSX] core init: cart ok\n");
    if (state->machineMode == MsxMachineMode::MSX2) {
        std::printf("[MSX] core init: vdp begin\n");
        if (!msx_vdp_init(&state->vdp, state->machineMode)) {
            std::printf("[MSX] core init failed at vdp init\n");
            msx_bios_shutdown(&state->bios);
            std::memset(&state->cart, 0, sizeof(state->cart));
            return false;
        }
        std::printf("[MSX] core init: vdp ok\n");
    }

    const size_t requestedRamSize =
        (state->machineMode == MsxMachineMode::MSX1) ? kMsxCartRamSizeMsx1
                                                     : msx_core_select_msx2_ram_size();
    std::printf("[MSX] core init: memory begin\n");
    std::printf("[MSX] core init: ram budget free=%u requested=%u\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(requestedRamSize));
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, requestedRamSize)) {
        std::printf("[MSX] core init failed at memory init\n");
        if (state->machineMode == MsxMachineMode::MSX2) {
            msx_vdp_shutdown(&state->vdp);
        }
        msx_bios_shutdown(&state->bios);
        std::memset(&state->cart, 0, sizeof(state->cart));
        return false;
    }

    std::printf("[MSX] core init: memory ok\n");
    if (state->machineMode != MsxMachineMode::MSX2) {
        std::printf("[MSX] core init: vdp begin\n");
        if (!msx_vdp_init(&state->vdp, state->machineMode)) {
            std::printf("[MSX] core init failed at vdp init\n");
            msx_memory_shutdown(&state->memory);
            msx_bios_shutdown(&state->bios);
            std::memset(&state->cart, 0, sizeof(state->cart));
            return false;
        }
        std::printf("[MSX] core init: vdp ok\n");
    }
    msx_core_init_audio(state, audioSampleRate);
    msx_core_attach_runtime_devices(state);

    state->bootPc = msx_core_select_boot_pc(&state->cart, &state->directBoot);
    msx_core_apply_boot_mapping(state,
                                state->directBoot ? kMsxBootSlotCart : kMsxBootSlotBios,
                                msx_core_default_secondary_slot_reg(state->machineMode, state->directBoot));

    msx_cpu_init(&state->cpu);
    msx_cpu_reset(&state->cpu, state->bootPc, kMsxDefaultStack);

    msx_vdp_render(&state->vdp);
    msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);

    state->initialized = true;
    msx_core_set_status(state,
                        "%s %04X %s %s",
                        state->directBoot ? "CART" : "BIOS",
                        state->bootPc,
                        msx_media_bios_target_label(state->biosTarget),
                        msx_vdp_mode_label(state->vdp.mode));
    msx_core_capture_status_state(state);

    MSX_CORE_LOG("[MSX] boot=%s pc=%04X mapper=%s bios=%s machine=%s ram=%u vram=%u audio=%s\n",
                 state->directBoot ? "cart" : "bios",
                 state->bootPc,
                 msx_media_cartridge_type_label(state->cart.type),
                 msx_media_bios_target_label(state->biosTarget),
                 msx_config_machine_mode_label(state->machineMode),
                 static_cast<unsigned>(state->memory.ramSize),
                 static_cast<unsigned>(state->vdp.vramSize),
                 state->audioHookReady ? "psg" : "off");
    return true;
}

void msx_core_handle_input(MsxCoreState* state, const MsxInputState* input)
{
    if (!state || !input || !state->initialized) {
        return;
    }

    uint8_t joy = 0xFFu;
    if (input->up)    { joy &= ~0x01u; }
    if (input->down)  { joy &= ~0x02u; }
    if (input->left)  { joy &= ~0x04u; }
    if (input->right) { joy &= ~0x08u; }
    if (input->fire1) { joy &= ~0x10u; }
    if (input->fire2) { joy &= ~0x20u; }

    uint8_t configFlags = 0u;
    if (input->joystickEnabled) {
        configFlags |= kMsxInputCfgJoy;
    }
    if (input->keyboardEnabled) {
        configFlags |= kMsxInputCfgKeyboard;
    }
    if (input->vausEnabled) {
        configFlags |= kMsxInputCfgVaus;
    }

    MsxKeyboardMatrix keyboardMatrix = input->keyboardMatrix;
    if (!input->keyboardEnabled) {
        msx_keyboard_matrix_clear(&keyboardMatrix);
    }

    uint8_t psgPortA = 0xFFu;
    uint8_t psgPortB = 0xFFu;
    if (input->joystickEnabled) {
        if (input->vausEnabled) {
            psgPortB = joy;
        } else {
            psgPortA = joy;
        }
    }

    const bool joyChanged = !state->lastInputCaptured || (state->lastInputJoy != joy);
    const bool configChanged =
        !state->lastInputCaptured || (state->lastInputConfigFlags != configFlags);
    const bool keyboardChanged =
        !state->lastInputCaptured ||
        (std::memcmp(state->lastInputKeyboardMatrix.rows,
                     keyboardMatrix.rows,
                     sizeof(keyboardMatrix.rows)) != 0);

    static uint16_t s_inputLogCount = 0u;
    if ((joyChanged || configChanged || keyboardChanged) && s_inputLogCount < 160u) {
        char pressed[8] = "------";
        if (input->up) { pressed[0] = 'U'; }
        if (input->down) { pressed[1] = 'D'; }
        if (input->left) { pressed[2] = 'L'; }
        if (input->right) { pressed[3] = 'R'; }
        if (input->fire1) { pressed[4] = 'A'; }
        if (input->fire2) { pressed[5] = 'B'; }

        char config[4] = "---";
        if (input->joystickEnabled) {
            config[0] = 'J';
        }
        if (input->keyboardEnabled) {
            config[1] = 'K';
        }
        if (input->vausEnabled) {
            config[2] = 'V';
        }

        char rowSummary[96];
        size_t used = 0u;
        bool anyRow = false;
        rowSummary[0] = '\0';
        for (uint8_t row = 0u; row < kMsxKeyboardRowCount; ++row) {
            const uint8_t rowValue = keyboardMatrix.rows[row];
            if (rowValue == 0xFFu) {
                continue;
            }

            anyRow = true;
            const int written = std::snprintf(rowSummary + used,
                                              sizeof(rowSummary) - used,
                                              "%s%u:%02X",
                                              (used != 0u) ? " " : "",
                                              static_cast<unsigned>(row),
                                              static_cast<unsigned>(rowValue));
            if (written <= 0) {
                break;
            }
            const size_t advance = static_cast<size_t>(written);
            if (advance >= (sizeof(rowSummary) - used)) {
                used = sizeof(rowSummary) - 1u;
                break;
            }
            used += advance;
        }
        if (!anyRow) {
            std::snprintf(rowSummary, sizeof(rowSummary), "idle");
        }

#if MSX_CORE_TRACE_ENABLED
        std::printf("[MSX][INPUT] cfg=%s joy=%02X psg=%02X/%02X keys=%s rows=%s #%u\n",
                    config,
                    static_cast<unsigned>(joy),
                    static_cast<unsigned>(psgPortA),
                    static_cast<unsigned>(psgPortB),
                    pressed,
                    rowSummary,
                    static_cast<unsigned>(s_inputLogCount));
        ++s_inputLogCount;
#endif
    }

    state->lastInputJoy = joy;
    state->lastInputConfigFlags = configFlags;
    state->lastInputJoystickMode = input->joystickMode;
    state->lastInputKeyboardMatrix = keyboardMatrix;
    state->lastInputCaptured = true;

    msx_memory_set_keyboard_matrix(&state->memory, &keyboardMatrix);

    if (state->audioHookReady) {
        msx_psg_set_vaus_enabled(&state->psg, input->vausEnabled);
        if (input->vausEnabled) {
            msx_psg_set_vaus_input(&state->psg,
                                   input->left,
                                   input->right,
                                   input->fire1 || input->fire2 || input->start);
        } else {
            msx_psg_set_vaus_input(&state->psg, false, false, false);
        }
        msx_psg_set_joysticks(&state->psg, psgPortA, psgPortB);
    }
}

void msx_core_step_frame(MsxCoreState* state)
{
    if (!state || !state->initialized) {
        return;
    }

    const bool vdpSliceMode = state->machineMode == MsxMachineMode::MSX2 &&
                              state->vdp.mode != MsxVdpMode::Unsupported &&
                              msx_vdp_display_enabled(&state->vdp);
    if (state->machineMode == MsxMachineMode::MSX2) {
        state->memory.cpu = &state->cpu;
        state->vdp.frameStartCpuCycles = state->cpu.totalCycles;
        state->vdp.currentFrameCpuCycles = 0u;
        state->vdp.frameCycleBudget = static_cast<uint32_t>(kMsxFrameCycles60Hz);
    }
    const bool irqEnabled = msx_vdp_begin_frame(&state->vdp);
    if (irqEnabled && !vdpSliceMode) {
        msx_cpu_request_irq(&state->cpu);
    }

    const MsxCpuRunState prevRunState = state->cpu.runState;
    if (vdpSliceMode) {
        const unsigned visibleLines = state->vdp.activeHeight != 0u ? state->vdp.activeHeight : 212u;
        const unsigned totalLines = 262u;
        const unsigned vblankLine = visibleLines > 192u ? 230u : 220u;
        uint32_t executedCycles = 0u;
        msx_vdp_prepare_frame_render(&state->vdp);
        state->vdp.status[0] &= static_cast<uint8_t>(~0x80u);
        state->vdp.status[1] &= static_cast<uint8_t>(~0x01u);
        state->vdp.status[2] &= static_cast<uint8_t>(~0x60u);
        for (unsigned line = 0; line < totalLines; ++line) {
            state->vdp.currentFrameCpuCycles = executedCycles;
            msx_vdp_advance_command_engine(&state->vdp, executedCycles);
            if (line < visibleLines) {
                msx_vdp_render_slice(&state->vdp, line, line + 1u, line + 1u == visibleLines);
            }
            const uint32_t targetCycles =
                static_cast<uint32_t>((static_cast<uint64_t>(line + 1u) *
                                       static_cast<uint64_t>(kMsxFrameCycles60Hz)) /
                                      static_cast<uint64_t>(totalLines));
            const int sliceBudget = targetCycles > executedCycles
                                        ? static_cast<int>(targetCycles - executedCycles)
                                        : 0;
            executedCycles += static_cast<uint32_t>(msx_cpu_run_cycles(&state->cpu, &state->memory, sliceBudget));
            state->vdp.currentFrameCpuCycles = executedCycles;
            if (line == static_cast<unsigned>(state->vdp.regs[19])) {
                state->vdp.status[1] |= 0x01u;
                if ((state->vdp.regs[0] & 0x10u) != 0u) {
                    msx_cpu_request_irq(&state->cpu);
                }
            }
            if (line + 1u == vblankLine) {
                state->vdp.status[0] |= 0x80u;
                if ((state->vdp.regs[1] & 0x20u) != 0u) {
                    msx_cpu_request_irq(&state->cpu);
                }
            }
        }
        uint64_t finalFrameCycles = state->cpu.totalCycles - state->vdp.frameStartCpuCycles;
        if (finalFrameCycles > static_cast<uint64_t>(state->vdp.frameCycleBudget)) {
            finalFrameCycles = static_cast<uint64_t>(state->vdp.frameCycleBudget);
        }
        state->vdp.currentFrameCpuCycles = static_cast<uint32_t>(finalFrameCycles);
        msx_vdp_advance_command_engine(&state->vdp, state->vdp.currentFrameCpuCycles);
        state->lastFrameCycles = executedCycles;
        state->vdp.dirty = false;
        state->vdp.frameReady = true;
        msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);
        if (state->displayFrame.indexed8) {
            msx_video_present_frame(&state->displayFrame);
        }
    } else {
        state->lastFrameCycles = static_cast<uint32_t>(msx_cpu_run_cycles(&state->cpu, &state->memory, kMsxFrameCycles60Hz));
    }

    // Log the first time the CPU enters a non-running state, and every 60 frames while stuck.
    const MsxCpuRunState curRunState = state->cpu.runState;
#if MSX_CORE_TRACE_ENABLED
    if (curRunState != MsxCpuRunState::Running) {
        if (prevRunState != curRunState || (state->frameCounter % 60u) == 0u) {
            std::printf("[MSX][CPU] state=%s pc=%04X op=%02X frame=%lu cycles=%lu\n",
                        msx_cpu_run_state_label(curRunState),
                        static_cast<unsigned>(state->cpu.pc),
                        static_cast<unsigned>(state->cpu.lastOpcode),
                        static_cast<unsigned long>(state->frameCounter),
                        static_cast<unsigned long>(state->lastFrameCycles));
        }
    } else if (state->frameCounter < 5u || (state->frameCounter % 60u) == 0u) {
        std::printf("[MSX][CPU] RUNNING pc=%04X vdp=%s irq=%d frame=%lu\n",
                    static_cast<unsigned>(state->cpu.pc),
                    msx_vdp_mode_label(state->vdp.mode),
                    static_cast<int>(irqEnabled),
                    static_cast<unsigned long>(state->frameCounter));
    }

    if (state->frameCounter == 5u || state->frameCounter == 60u || state->frameCounter == 120u) {
        std::printf("[MSX][VDP] DUMP frame=%lu slot=0x%02X  R0=%02X R1=%02X R2=%02X R3=%02X R4=%02X R5=%02X R6=%02X R7=%02X\n",
                    static_cast<unsigned long>(state->frameCounter),
                    static_cast<unsigned>(state->memory.slotRegister),
                    state->vdp.regs[0], state->vdp.regs[1],
                    state->vdp.regs[2], state->vdp.regs[3],
                    state->vdp.regs[4], state->vdp.regs[5],
                    state->vdp.regs[6], state->vdp.regs[7]);
    }
#endif

    if (!vdpSliceMode) {
        msx_vdp_render(&state->vdp);
        msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);
    }
    state->frameCounter++;

    if (msx_core_status_needs_refresh(state)) {
        msx_core_refresh_status(state);
    }
}

size_t msx_core_drain_audio(MsxCoreState* state, int16_t* dst, size_t capacity)
{
    if (!state || !state->initialized || !state->audioHookReady) {
        return 0u;
    }

    if (!dst || capacity == 0u) {
        const size_t available = msx_psg_available_samples(&state->psg);
        msx_psg_discard_samples(&state->psg, available);
        state->lastAudioSamples = 0u;
        return 0u;
    }

    const size_t sampleCount = msx_psg_read_samples(&state->psg, dst, capacity);
    state->lastAudioSamples = static_cast<uint16_t>(sampleCount);
    return sampleCount;
}

void msx_core_shutdown(MsxCoreState* state)
{
    if (!state) {
        return;
    }

    msx_psg_shutdown(&state->psg);
    msx_memory_shutdown(&state->memory);
    msx_vdp_shutdown(&state->vdp);
    msx_bios_shutdown(&state->bios);
    std::memset(state, 0, sizeof(*state));
}

void msx_core_attach_disk_rom(MsxCoreState* state,
                              const uint8_t* diskRomData,
                              size_t diskRomSize)
{
    if (!state || !state->memory.ready) {
        return;
    }

    state->memory.diskRom = diskRomData;
    state->memory.diskRomSize = diskRomSize;
    state->memory.diskPatch = msx_disk_bios_patch_handler;
    msx_memory_refresh_maps(&state->memory);
    msx_core_seed_slot_work_area(state, state->memory.secondarySlotRegs[3]);

    std::printf("[MSX] core attach_disk_rom: %s size=%u\n",
                diskRomData ? "attached" : "none",
                static_cast<unsigned>(diskRomSize));
}

bool msx_core_init_basic(MsxCoreState* state,
                         const MsxBiosBundle* bios,
                         const char* name,
                         uint32_t audioSampleRate)
{
    if (!state || !bios || !bios->compatible) {
        std::printf("[MSX] core init_basic failed: invalid bios\n");
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->biosTarget = bios->target;
    state->machineMode = msx_media_target_to_machine_mode(bios->target);
    state->videoHookReady = true;
    std::snprintf(state->romName, sizeof(state->romName),
                  "%s", (name && name[0] != '\0') ? name : "MSX BASIC");

    std::printf("[MSX] core init_basic: bios begin\n");
    if (!msx_bios_init(&state->bios, bios)) {
        std::printf("[MSX] core init_basic failed at bios init\n");
        return false;
    }

    std::memset(&state->cart, 0, sizeof(state->cart));

    std::printf("[MSX] core init_basic: memory begin\n");
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, 0u)) {
        std::printf("[MSX] core init_basic failed at memory init\n");
        msx_bios_shutdown(&state->bios);
        return false;
    }

    std::printf("[MSX] core init_basic: vdp begin\n");
    if (!msx_vdp_init(&state->vdp, state->machineMode)) {
        std::printf("[MSX] core init_basic failed at vdp init\n");
        msx_memory_shutdown(&state->memory);
        msx_bios_shutdown(&state->bios);
        return false;
    }

    msx_core_init_audio(state, audioSampleRate);
    msx_core_finish_no_cart_init(state);
    msx_core_set_status(state,
                        "BASIC %s",
                        msx_media_bios_target_label(state->biosTarget));
    std::printf("[MSX] core init_basic ok\n");
    return true;
}

bool msx_core_init_disk(MsxCoreState* state,
                        const MsxBiosBundle* bios,
                        const uint8_t* diskRomData, size_t diskRomSize,
                        const uint8_t* dskData, size_t dskSize,
                        const char* name,
                        uint32_t audioSampleRate)
{
    if (!state || !bios || !bios->compatible) {
        std::printf("[MSX] core init_disk failed: invalid bios\n");
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->biosTarget = bios->target;
    state->machineMode = msx_media_target_to_machine_mode(bios->target);
    state->videoHookReady = true;
    std::snprintf(state->romName, sizeof(state->romName),
                  "%s", (name && name[0] != '\0') ? name : "MSX DISK");

    std::printf("[MSX] core init_disk: bios begin\n");
    if (!msx_bios_init(&state->bios, bios)) {
        std::printf("[MSX] core init_disk failed at bios init\n");
        return false;
    }

    std::memset(&state->cart, 0, sizeof(state->cart));

    std::printf("[MSX] core init_disk: memory begin\n");
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, 0u)) {
        std::printf("[MSX] core init_disk failed at memory init\n");
        msx_bios_shutdown(&state->bios);
        return false;
    }

    std::printf("[MSX] core init_disk: vdp begin\n");
    if (!msx_vdp_init(&state->vdp, state->machineMode)) {
        std::printf("[MSX] core init_disk failed at vdp init\n");
        msx_memory_shutdown(&state->memory);
        msx_bios_shutdown(&state->bios);
        return false;
    }

    msx_core_init_audio(state, audioSampleRate);
    state->memory.diskRom = diskRomData;
    state->memory.diskRomSize = diskRomSize;
    state->memory.diskPatch = msx_disk_bios_patch_handler;
    msx_disk_init(&state->disk, dskData, dskSize);
    state->memory.disk = &state->disk;
    std::printf("[MSX] core init_disk: diskRom=%s diskSize=%u inferredSides=%u\n",
                diskRomData ? "yes" : "no",
                static_cast<unsigned>(dskSize),
                static_cast<unsigned>(state->disk.sides));

    msx_core_finish_no_cart_init(state);
    if (diskRomData) {
        if (!msx_core_try_boot_disk_sector(state)) {
            std::printf("[MSX] disk boot: staying on BIOS entry path\n");
        }
    }
    msx_core_set_status(state,
                        "DISK %s %s",
                        diskRomData ? "DISK.ROM" : "noDISK",
                        msx_media_bios_target_label(state->biosTarget));
    std::printf("[MSX] core init_disk ok (diskRom=%s dsk=%u B sides=%u)\n",
                diskRomData ? "yes" : "no",
                static_cast<unsigned>(dskSize),
                static_cast<unsigned>(state->disk.sides));
    return true;
}

bool msx_core_init_cas(MsxCoreState* state,
                       const MsxBiosBundle* bios,
                       const uint8_t* casData, size_t casSize,
                       const char* name,
                       uint32_t audioSampleRate)
{
    if (!state || !bios || !bios->compatible) {
        std::printf("[MSX] core init_cas failed: invalid bios\n");
        return false;
    }

    std::memset(state, 0, sizeof(*state));
    state->biosTarget = bios->target;
    state->machineMode = msx_media_target_to_machine_mode(bios->target);
    state->videoHookReady = true;
    std::snprintf(state->romName, sizeof(state->romName),
                  "%s", (name && name[0] != '\0') ? name : "MSX CAS");

    std::printf("[MSX] core init_cas: bios begin\n");
    if (!msx_bios_init(&state->bios, bios)) {
        std::printf("[MSX] core init_cas failed at bios init\n");
        return false;
    }

    std::memset(&state->cart, 0, sizeof(state->cart));

    std::printf("[MSX] core init_cas: memory begin\n");
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, 0u)) {
        std::printf("[MSX] core init_cas failed at memory init\n");
        msx_bios_shutdown(&state->bios);
        return false;
    }

    std::printf("[MSX] core init_cas: vdp begin\n");
    if (!msx_vdp_init(&state->vdp, state->machineMode)) {
        std::printf("[MSX] core init_cas failed at vdp init\n");
        msx_memory_shutdown(&state->memory);
        msx_bios_shutdown(&state->bios);
        return false;
    }

    msx_cas_init(&state->cas, casData, casSize);
    state->memory.cas = &state->cas;
    std::printf("[MSX] core init_cas: cas size=%u ready=%s\n",
                static_cast<unsigned>(casSize),
                state->cas.ready ? "yes" : "no");

    msx_core_init_audio(state, audioSampleRate);
    msx_core_finish_no_cart_init(state);
    msx_core_set_status(state,
                        "CAS %s",
                        msx_media_bios_target_label(state->biosTarget));
    std::printf("[MSX] core init_cas ok\n");
    return true;
}

bool msx_core_change_cas(MsxCoreState* state,
                         const uint8_t* casData, size_t casSize,
                         const char* name)
{
    if (!state || !state->initialized) {
        std::printf("[MSX] core change_cas failed: core not initialized\n");
        return false;
    }

    msx_cas_init(&state->cas, casData, casSize);
    state->memory.cas = &state->cas;
    std::snprintf(state->romName, sizeof(state->romName),
                  "%s", (name && name[0] != '\0') ? name : "MSX CAS");
    msx_core_set_status(state,
                        "CAS %s",
                        state->cas.ready ? "changed" : "not ready");
    std::printf("[MSX] core change_cas: name=%s size=%u ready=%s\n",
                state->romName,
                static_cast<unsigned>(casSize),
                state->cas.ready ? "yes" : "no");
    return state->cas.ready;
}

bool msx_core_save_state(MsxCoreState* state, const char* path)
{
    if (!state) return false;

    std::printf("[MSX][STATE] Saving state to %s\n", path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        std::printf("[MSX][STATE] Error: could not open file for writing\n");
        return false;
    }

    uint32_t magic = 0x4D535853; // "MSXS"
    f.write((uint8_t*)&magic, 4);

    f.write((uint8_t*)&state->cpu, sizeof(MsxCpuState));
    f.write((uint8_t*)&state->vdp, sizeof(MsxVdpState));
    f.write((uint8_t*)&state->psg, sizeof(MsxPsgState));
    f.write((uint8_t*)&state->memory, sizeof(MsxMemoryState));
    f.write((uint8_t*)&state->cart, sizeof(MsxCartState));
    f.write((uint8_t*)&state->disk, sizeof(MsxDiskState));

    if (state->vdp.vramSize > 0) {
        f.write(state->vdp.vram, state->vdp.vramSize);
    }

    for (uint8_t i = 0; i < state->memory.ramBankCount; ++i) {
        if (state->memory.ramBanks[i]) {
            f.write(state->memory.ramBanks[i], 8192);
        }
    }

    f.close();
    std::printf("[MSX][STATE] Save completed successfully\n");
    return true;
}

bool msx_core_load_state(MsxCoreState* state, const char* path)
{
    if (!state) return false;

    std::printf("[MSX][STATE] Loading state from %s\n", path);
    File f = SD.open(path, FILE_READ);
    if (!f) {
        std::printf("[MSX][STATE] Error: could not open file for reading\n");
        return false;
    }

    uint32_t magic = 0;
    f.read((uint8_t*)&magic, 4);
    if (magic != 0x4D535853) {
        std::printf("[MSX][STATE] Error: invalid magic signature %08X\n", static_cast<unsigned>(magic));
        f.close();
        return false;
    }

    // Save hardware mapping pointers before overwriting the memory state blocks
    uint8_t* vramPtr = state->vdp.vram;
    uint8_t* fbPtr = state->vdp.frameBuffer;
    int16_t* ringPtr = state->psg.ring;
    uint8_t* banks[16];
    for (int i = 0; i < 16; i++) banks[i] = state->memory.ramBanks[i];
    const uint8_t* diskRom = state->memory.diskRom;
    MsxDiskState* disk = state->memory.disk;
    void* patch = (void*)state->memory.diskPatch;
    MsxVdpState* vdp = state->memory.vdp;
    MsxPsgState* psg = state->memory.psg;
    const uint8_t* biosMain = state->memory.bios.mainRom;
    const uint8_t* biosSub = state->memory.bios.subRom;
    const uint8_t* cartRom = state->memory.cart.rom;
    const uint8_t* diskDsk = state->disk.dskData;
    MsxCasState casState = state->cas;

    f.read((uint8_t*)&state->cpu, sizeof(MsxCpuState));
    f.read((uint8_t*)&state->vdp, sizeof(MsxVdpState));
    f.read((uint8_t*)&state->psg, sizeof(MsxPsgState));
    f.read((uint8_t*)&state->memory, sizeof(MsxMemoryState));
    f.read((uint8_t*)&state->cart, sizeof(MsxCartState));
    f.read((uint8_t*)&state->disk, sizeof(MsxDiskState));

    // Restore pointers
    state->vdp.vram = vramPtr;
    state->vdp.frameBuffer = fbPtr;
    state->psg.ring = ringPtr;
    for (int i = 0; i < 16; i++) state->memory.ramBanks[i] = banks[i];
    state->memory.diskRom = diskRom;
    state->memory.disk = disk;
    state->memory.diskPatch = (void(*)(MsxCpuState*, MsxMemoryState*, uint16_t))patch;
    state->memory.vdp = vdp;
    state->memory.psg = psg;
    state->memory.bios.mainRom = biosMain;
    state->memory.bios.subRom = biosSub;
    state->memory.cart.rom = cartRom;
    state->cart.rom = cartRom;
    state->disk.dskData = diskDsk;
    state->cas = casState;
    state->memory.cas = &state->cas;

    if (state->vdp.vramSize > 0) {
        f.read(state->vdp.vram, state->vdp.vramSize);
    }

    for (uint8_t i = 0; i < state->memory.ramBankCount; ++i) {
        if (state->memory.ramBanks[i]) {
            f.read(state->memory.ramBanks[i], 8192);
        }
    }

    f.close();

    state->vdp.dirty = true;
    msx_memory_refresh_maps(&state->memory);
    std::printf("[MSX][STATE] Load completed successfully\n");
    return true;
}

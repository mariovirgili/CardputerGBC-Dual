#include "msx_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "msx_disk.h"

#ifndef MSX_CORE_LOG_ENABLED
#define MSX_CORE_LOG_ENABLED 0
#endif

#if MSX_CORE_LOG_ENABLED
#define MSX_CORE_LOG(...) std::printf(__VA_ARGS__)
#else
#define MSX_CORE_LOG(...) do { } while (0)
#endif

namespace {

constexpr int kMsxFrameCycles60Hz = 59659;
constexpr uint8_t kMsxBootSlotBios = 0xD0;
constexpr uint8_t kMsxBootSlotCart = 0xD4;
constexpr uint8_t kMsxBootSlotDisk = 0xF8;
constexpr uint16_t kMsxDefaultStack = 0xF380;
constexpr uint16_t kMsxDiskBootAddress = 0xC000;
constexpr uint32_t kMsxStatusRefreshPeriod = 8u;
constexpr size_t kMsxCartRamSizeMsx1 = 0x8000u;

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

    if (cart->entryPoint >= 0x4000u && cart->entryPoint < 0xC000u) {
        if (directBoot) {
            *directBoot = true;
        }
        return cart->entryPoint;
    }

    if (cart->initAddress >= 0x4000u && cart->initAddress < 0xC000u) {
        if (directBoot) {
            *directBoot = true;
        }
        return cart->initAddress;
    }

    return 0x0000u;
}

void msx_core_attach_runtime_devices(MsxCoreState* state)
{
    if (!state) {
        return;
    }

    msx_memory_attach_vdp(&state->memory, &state->vdp);
    if (state->audioHookReady) {
        msx_memory_attach_psg(&state->memory, &state->psg);
        msx_psg_reset(&state->psg);
    } else {
        msx_memory_attach_psg(&state->memory, nullptr);
    }
}

void msx_core_finish_no_cart_init(MsxCoreState* state)
{
    msx_core_attach_runtime_devices(state);
    state->memory.slotRegister = kMsxBootSlotBios;
    state->memory.lastPortA8 = kMsxBootSlotBios;
    msx_memory_refresh_maps(&state->memory);

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

    const uint8_t jump = sector[0];
    if (jump != 0xEBu && jump != 0xE9u && jump != 0xC3u) {
        std::printf("[MSX] disk boot: sector 0 is not executable (opcode=%02X)\n",
                    static_cast<unsigned>(jump));
        return false;
    }

    state->memory.slotRegister = kMsxBootSlotDisk;
    state->memory.lastPortA8 = kMsxBootSlotDisk;
    msx_memory_refresh_maps(&state->memory);

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

    std::printf("[MSX] core init: bios ok\n");
    std::printf("[MSX] core init: cart begin\n");
    if (!msx_cart_init(&state->cart, rom)) {
        std::printf("[MSX] core init failed at cart init\n");
        msx_bios_shutdown(&state->bios);
        return false;
    }

    std::printf("[MSX] core init: cart ok\n");
    std::printf("[MSX] core init: memory begin\n");
    const size_t requestedRamSize =
        (state->machineMode == MsxMachineMode::MSX1) ? kMsxCartRamSizeMsx1 : 0u;
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, requestedRamSize)) {
        std::printf("[MSX] core init failed at memory init\n");
        msx_bios_shutdown(&state->bios);
        std::memset(&state->cart, 0, sizeof(state->cart));
        return false;
    }

    std::printf("[MSX] core init: memory ok\n");
    std::printf("[MSX] core init: vdp begin\n");
    if (!msx_vdp_init(&state->vdp, state->machineMode)) {
        std::printf("[MSX] core init failed at vdp init\n");
        msx_memory_shutdown(&state->memory);
        msx_bios_shutdown(&state->bios);
        std::memset(&state->cart, 0, sizeof(state->cart));
        return false;
    }

    std::printf("[MSX] core init: vdp ok\n");
    msx_core_init_audio(state, audioSampleRate);
    msx_core_attach_runtime_devices(state);

    state->bootPc = msx_core_select_boot_pc(&state->cart, &state->directBoot);
    state->memory.slotRegister = state->directBoot ? kMsxBootSlotCart : kMsxBootSlotBios;
    state->memory.lastPortA8 = state->memory.slotRegister;
    msx_memory_refresh_maps(&state->memory);

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

    msx_memory_set_keyboard_matrix(&state->memory, &input->keyboardMatrix);
}

void msx_core_step_frame(MsxCoreState* state)
{
    if (!state || !state->initialized) {
        return;
    }

    const bool irqEnabled = msx_vdp_begin_frame(&state->vdp);
    if (irqEnabled || state->cpu.halted) {
        msx_cpu_request_irq(&state->cpu);
    }

    state->lastFrameCycles = static_cast<uint32_t>(msx_cpu_run_cycles(&state->cpu, &state->memory, kMsxFrameCycles60Hz));
    msx_vdp_render(&state->vdp);
    msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);
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
    state->memory.diskPatch = diskRomData ? msx_disk_bios_patch_handler : nullptr;
    msx_memory_refresh_maps(&state->memory);

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
    state->memory.diskPatch = diskRomData ? msx_disk_bios_patch_handler : nullptr;
    msx_disk_init(&state->disk, dskData, dskSize);
    state->memory.disk = &state->disk;

    msx_core_finish_no_cart_init(state);
    if (diskRomData) {
        msx_core_try_boot_disk_sector(state);
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


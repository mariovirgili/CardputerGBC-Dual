#include "msx_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "msx_disk.h"
#include "../msx_logging.h"
#include "../msx_config.h"
#include "../msx_sound.h"
#include "../msx_video.h"
#include "../../bench/v9938_bench_trace.hpp"

#ifndef MSX_CORE_LOG_ENABLED
#define MSX_CORE_LOG_ENABLED 1
#endif

#ifndef MSX_BOOTSTRAP_LOG_ENABLED
#define MSX_BOOTSTRAP_LOG_ENABLED 1
#endif

#ifndef MSX_CORE_TIMING_ENABLED
#define MSX_CORE_TIMING_ENABLED 0
#endif

#if MSX_CORE_LOG_ENABLED
#define MSX_CORE_LOG(...) MSX_RUNTIME_LOG(__VA_ARGS__)
#else
#define MSX_CORE_LOG(...) do { } while (0)
#endif

namespace {

constexpr int kMsxFrameCycles60Hz = 59659;
constexpr unsigned kMsxTotalScanlines60Hz = 262u;
constexpr uint16_t kMsxScanlineTargetCycles60Hz[kMsxTotalScanlines60Hz] = {
    227u, 455u, 683u, 910u, 1138u, 1366u, 1593u, 1821u, 2049u, 2277u, 2504u, 2732u,
    2960u, 3187u, 3415u, 3643u, 3871u, 4098u, 4326u, 4554u, 4781u, 5009u, 5237u, 5464u,
    5692u, 5920u, 6148u, 6375u, 6603u, 6831u, 7058u, 7286u, 7514u, 7742u, 7969u, 8197u,
    8425u, 8652u, 8880u, 9108u, 9335u, 9563u, 9791u, 10019u, 10246u, 10474u, 10702u, 10929u,
    11157u, 11385u, 11613u, 11840u, 12068u, 12296u, 12523u, 12751u, 12979u, 13206u, 13434u, 13662u,
    13890u, 14117u, 14345u, 14573u, 14800u, 15028u, 15256u, 15484u, 15711u, 15939u, 16167u, 16394u,
    16622u, 16850u, 17077u, 17305u, 17533u, 17761u, 17988u, 18216u, 18444u, 18671u, 18899u, 19127u,
    19355u, 19582u, 19810u, 20038u, 20265u, 20493u, 20721u, 20948u, 21176u, 21404u, 21632u, 21859u,
    22087u, 22315u, 22542u, 22770u, 22998u, 23226u, 23453u, 23681u, 23909u, 24136u, 24364u, 24592u,
    24819u, 25047u, 25275u, 25503u, 25730u, 25958u, 26186u, 26413u, 26641u, 26869u, 27097u, 27324u,
    27552u, 27780u, 28007u, 28235u, 28463u, 28690u, 28918u, 29146u, 29374u, 29601u, 29829u, 30057u,
    30284u, 30512u, 30740u, 30968u, 31195u, 31423u, 31651u, 31878u, 32106u, 32334u, 32561u, 32789u,
    33017u, 33245u, 33472u, 33700u, 33928u, 34155u, 34383u, 34611u, 34839u, 35066u, 35294u, 35522u,
    35749u, 35977u, 36205u, 36432u, 36660u, 36888u, 37116u, 37343u, 37571u, 37799u, 38026u, 38254u,
    38482u, 38710u, 38937u, 39165u, 39393u, 39620u, 39848u, 40076u, 40303u, 40531u, 40759u, 40987u,
    41214u, 41442u, 41670u, 41897u, 42125u, 42353u, 42581u, 42808u, 43036u, 43264u, 43491u, 43719u,
    43947u, 44174u, 44402u, 44630u, 44858u, 45085u, 45313u, 45541u, 45768u, 45996u, 46224u, 46452u,
    46679u, 46907u, 47135u, 47362u, 47590u, 47818u, 48045u, 48273u, 48501u, 48729u, 48956u, 49184u,
    49412u, 49639u, 49867u, 50095u, 50323u, 50550u, 50778u, 51006u, 51233u, 51461u, 51689u, 51916u,
    52144u, 52372u, 52600u, 52827u, 53055u, 53283u, 53510u, 53738u, 53966u, 54194u, 54421u, 54649u,
    54877u, 55104u, 55332u, 55560u, 55787u, 56015u, 56243u, 56471u, 56698u, 56926u, 57154u, 57381u,
    57609u, 57837u, 58065u, 58292u, 58520u, 58748u, 58975u, 59203u, 59431u, 59659u
};
static_assert(kMsxScanlineTargetCycles60Hz[kMsxTotalScanlines60Hz - 1u] == kMsxFrameCycles60Hz,
              "MSX scanline target table must end at the frame cycle budget");
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
constexpr uint8_t kMsxSlotIdMainRamExpanded = 0x8Bu;   // expanded slot 3-2
constexpr uint8_t kMsxSlotIdMainRamPrimary = 0x03u;    // primary slot 3, not expanded
constexpr uint8_t kMsxSlotIdDiskRom = 0x87u;   // expanded slot 3-1
constexpr uint8_t kMsxSlotIdCartridge = 0x01u; // primary slot 1
constexpr uint8_t kMsxInputCfgJoy = 0x01u;
constexpr uint8_t kMsxInputCfgKeyboard = 0x02u;
constexpr uint8_t kMsxInputCfgVaus = 0x04u;
constexpr uint16_t kMsxAddrExptbl = 0xFCC1u;
constexpr uint16_t kMsxAddrSlttbl = 0xFCC5u;
constexpr uint16_t kMsxAddrSltatr = 0xFCCCu;  // slot attribute table (60 bytes)
constexpr uint16_t kMsxAddrSltwrk = 0xFD09u;  // slot work area (128 bytes)
constexpr uint32_t kMsxSaveChunkScc = 0x53434353u; // "SCCS"
constexpr uint16_t kMsxSaveSccVersion = 1u;

struct MsxSaveChunkHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t size;
};

struct MsxSccSavePayloadV1 {
    uint8_t regs[256];
    uint32_t phase[5];
    uint32_t step[5];
    uint32_t sampleRate;
    uint32_t cpuClockHz;
    uint64_t sampleAccumulator;
    uint32_t generatedSamples;
    uint32_t droppedSamples;
    uint32_t audibleSamples;
    uint32_t lastAudibleLogSample;
    uint16_t audiblePeak;
    uint8_t audibleLogCount;
    uint8_t classicWindow;
    uint8_t plusWindow;
    uint8_t sccPlusMode;
    uint8_t enabled;
    uint8_t outputEnabled;
    uint8_t ready;
    uint8_t realClassicWindow;
    uint8_t realPlusWindow;
    uint8_t reserved[5];
};

struct MsxProfileWindow {
    uint32_t frames;
    uint32_t worstFrameUs;
    uint64_t frameUs;
    uint64_t cpuUs;
    uint64_t vdpUs;
    uint64_t presentUs;
};

static MsxProfileWindow s_msxProfileWindow = {};
static uint32_t s_msxProfileLastAudioDrops = 0u;

uint32_t msx_profile_avg_us(uint64_t totalUs, uint32_t frames)
{
    return frames != 0u ? static_cast<uint32_t>(totalUs / frames) : 0u;
}

uint32_t msx_profile_percent(uint64_t partUs, uint64_t totalUs)
{
    return totalUs != 0u
               ? static_cast<uint32_t>((partUs * 100u + (totalUs / 2u)) / totalUs)
               : 0u;
}

void msx_core_log_profile(MsxCoreState* state,
                          uint32_t frameUs,
                          uint32_t cpuUs,
                          uint32_t vdpUs,
                          uint32_t presentUs,
                          bool vdpSliceMode)
{
    if (!state || !msx_logs_enabled()) {
        return;
    }

    s_msxProfileWindow.frames++;
    s_msxProfileWindow.frameUs += frameUs;
    s_msxProfileWindow.cpuUs += cpuUs;
    s_msxProfileWindow.vdpUs += vdpUs;
    s_msxProfileWindow.presentUs += presentUs;
    if (frameUs > s_msxProfileWindow.worstFrameUs) {
        s_msxProfileWindow.worstFrameUs = frameUs;
    }

    if (s_msxProfileWindow.frames < MSX_PROFILE_LOG_INTERVAL_FRAMES) {
        return;
    }

    const uint32_t avgFrameUs = msx_profile_avg_us(s_msxProfileWindow.frameUs, s_msxProfileWindow.frames);
    const uint32_t avgCpuUs = msx_profile_avg_us(s_msxProfileWindow.cpuUs, s_msxProfileWindow.frames);
    const uint32_t avgVdpUs = msx_profile_avg_us(s_msxProfileWindow.vdpUs, s_msxProfileWindow.frames);
    const uint32_t avgPresentUs = msx_profile_avg_us(s_msxProfileWindow.presentUs, s_msxProfileWindow.frames);
    const uint32_t accountedUs = avgCpuUs + avgVdpUs + avgPresentUs;
    const uint32_t avgOtherUs = avgFrameUs > accountedUs ? (avgFrameUs - accountedUs) : 0u;
    const MsxAudioHookState& audio = msx_sound_get_state();
    const MsxVideoPerfSummary video = msx_video_get_perf_summary();
    const uint32_t audioDropsDelta = audio.droppedFrames - s_msxProfileLastAudioDrops;
    const uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const uint32_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    MSX_CATEGORY_LOG(MsxLogCategory::Profile,
                "[MSX][PROFILE] %uf frame=%u us cpu=%u(%u%%) vdp=%u(%u%%) present=%u(%u%%) other=%u(%u%%) max=%u us slice=%u mode=%s machine=%s heap8=%u largest8=%u int=%u largestInt=%u audioDrops=%lu(+%u) queued=%u samples=%u videoAvg=%u videoWorst=%u videoSkip=%u videoFail=%u fs=%u%%\n",
                static_cast<unsigned>(s_msxProfileWindow.frames),
                static_cast<unsigned>(avgFrameUs),
                static_cast<unsigned>(avgCpuUs),
                static_cast<unsigned>(msx_profile_percent(avgCpuUs, avgFrameUs)),
                static_cast<unsigned>(avgVdpUs),
                static_cast<unsigned>(msx_profile_percent(avgVdpUs, avgFrameUs)),
                static_cast<unsigned>(avgPresentUs),
                static_cast<unsigned>(msx_profile_percent(avgPresentUs, avgFrameUs)),
                static_cast<unsigned>(avgOtherUs),
                static_cast<unsigned>(msx_profile_percent(avgOtherUs, avgFrameUs)),
                static_cast<unsigned>(s_msxProfileWindow.worstFrameUs),
                static_cast<unsigned>(vdpSliceMode ? 1u : 0u),
                msx_vdp_mode_label(state->vdp.mode),
                msx_config_machine_mode_label(state->machineMode),
                static_cast<unsigned>(free8),
                static_cast<unsigned>(largest8),
                static_cast<unsigned>(freeInternal),
                static_cast<unsigned>(largestInternal),
                static_cast<unsigned long>(audio.droppedFrames),
                static_cast<unsigned>(audioDropsDelta),
                static_cast<unsigned>(audio.queuedBlocks),
                static_cast<unsigned>(state->lastAudioSamples),
                static_cast<unsigned>(video.valid ? video.avgPresentUs : 0u),
                static_cast<unsigned>(video.valid ? video.worstPresentUs : 0u),
                static_cast<unsigned>(video.valid ? video.skippedFrames : 0u),
                static_cast<unsigned>(video.valid ? video.presentFails : 0u),
                static_cast<unsigned>(video.valid ? video.frameskipPercent : 0u));

    s_msxProfileLastAudioDrops = audio.droppedFrames;
    s_msxProfileWindow = {};
}
constexpr uint16_t kMsxAddrDrvInv = 0xFB21u;
constexpr uint16_t kMsxAddrRamAd0 = 0xF341u;
constexpr uint16_t kMsxAddrMaster = 0xF348u;
constexpr size_t kMsxRamSizeMsx2 = 0x20000u;
constexpr size_t kMsxRamSizeMsx1 = 0x10000u;
constexpr size_t kMsxPageSize8K = 0x2000u;
constexpr size_t kMsxPageSize16K = 0x4000u;
constexpr size_t kMsx2VramSize = 0x20000u;
constexpr size_t kMsxCoreInitReserve = 0x4000u;

size_t msx_core_select_msx2_ram_size(const uint8_t* mainRom, size_t reserveBytes)
{
    const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t dynamicBudget = (free8 > freeInternal) ? free8 : freeInternal;
    const size_t largestDynamicBlock = (largest8 > largestInternal) ? largest8 : largestInternal;
    const uint8_t staticBanks = msx_media_static_ram_bank_count_for_main_bios(mainRom);
    size_t staticBytes = static_cast<size_t>(staticBanks) * kMsxPageSize8K;
    if (staticBytes > kMsxRamSizeMsx2) {
        staticBytes = kMsxRamSizeMsx2;
    }

    const size_t dynamicAfterReserve =
        (dynamicBudget > reserveBytes) ? (dynamicBudget - reserveBytes) : 0u;
    const size_t largestAfterReserve =
        (largestDynamicBlock > reserveBytes) ? (largestDynamicBlock - reserveBytes) : 0u;
    const size_t totalBudget = staticBytes + dynamicAfterReserve;
    const size_t candidates[] = {
        kMsxRamSizeMsx2,
        kMsxRamSizeMsx2 - kMsxPageSize16K,
        kMsxRamSizeMsx2 - (2u * kMsxPageSize16K),
        kMsxRamSizeMsx2 - (3u * kMsxPageSize16K),
        kMsxRamSizeMsx1
    };
    size_t selectedRam = kMsxRamSizeMsx1;
    size_t selectedRequired = kMsxPageSize8K;

    for (size_t candidate : candidates) {
        const size_t targetBanks = candidate / kMsxPageSize8K;
        const size_t staticBankBytes = static_cast<size_t>(staticBanks) * kMsxPageSize8K;
        const size_t dynamicBanks =
            (targetBanks > staticBanks) ? (targetBanks - staticBanks) : 0u;
        const size_t requiredContiguous = dynamicBanks * kMsxPageSize8K;
        if (totalBudget < candidate) {
            continue;
        }
        if (largestAfterReserve < requiredContiguous) {
            continue;
        }
        selectedRam = candidate;
        selectedRequired = requiredContiguous;
        (void)staticBankBytes;
        break;
    }

    std::printf("[MSX] core init: msx2 ram select freeInt=%u free8=%u largestInt=%u largest8=%u static=%u reserve=%u budget=%u largest=%u required=%u selected=%u\n",
                static_cast<unsigned>(freeInternal),
                static_cast<unsigned>(free8),
                static_cast<unsigned>(largestInternal),
                static_cast<unsigned>(largest8),
                static_cast<unsigned>(staticBytes),
                static_cast<unsigned>(reserveBytes),
                static_cast<unsigned>(totalBudget),
                static_cast<unsigned>(largestAfterReserve),
                static_cast<unsigned>(selectedRequired),
                static_cast<unsigned>(selectedRam));
    return selectedRam;
}

size_t msx_core_select_system_ram_size(MsxMachineMode machineMode)
{
    return (machineMode == MsxMachineMode::MSX1) ? kMsxRamSizeMsx1
                                                 : kMsxRamSizeMsx2;
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

#if MSX_BOOTSTRAP_LOG_ENABLED
uint8_t msx_core_slot_for_page(uint8_t slotRegister, uint8_t pageIndex)
{
    return static_cast<uint8_t>((slotRegister >> (pageIndex * 2u)) & 0x03u);
}

bool msx_core_bootstrap_frame_should_log(const MsxCoreState* state)
{
    if (!state || !msx_log_category_enabled(MsxLogCategory::Bootstrap)) {
        return false;
    }
    if (state->frameCounter < 20u) {
        return true;
    }
    if (state->frameCounter < 180u && (state->frameCounter % 10u) == 0u) {
        return true;
    }
    if (state->frameCounter <= 360u && (state->frameCounter % 60u) == 0u) {
        return true;
    }
    return false;
}

void msx_core_log_bootstrap_frame(const MsxCoreState* state,
                                  bool vdpSliceMode,
                                  uint32_t executedCycles,
                                  bool irqEnabled)
{
    if (!msx_core_bootstrap_frame_should_log(state)) {
        return;
    }

    const MsxVdpState& vdp = state->vdp;
    const MsxCartState& cart = state->memory.cart;
    std::printf("[MSX][BOOTDBG] frame=%lu pc=%04X last=%04X op=%02X sp=%04X af=%04X bc=%04X de=%04X hl=%04X "
                "run=%s irq=%u pend=%u iff=%u cyc=%lu slice=%u slot=%02X p=%u/%u/%u/%u ssl3=%02X "
                "banks=%u/%u/%u/%u mode=%s h=%u disp=%u R0=%02X R1=%02X R2=%02X R7=%02X R9=%02X "
                "R18=%02X R23=%02X R25=%02X R26=%02X R27=%02X S0=%02X S1=%02X S2=%02X\n",
                static_cast<unsigned long>(state->frameCounter),
                static_cast<unsigned>(state->cpu.pc),
                static_cast<unsigned>(state->cpu.lastPc),
                static_cast<unsigned>(state->cpu.lastOpcode),
                static_cast<unsigned>(state->cpu.sp),
                static_cast<unsigned>(state->cpu.af),
                static_cast<unsigned>(state->cpu.bc),
                static_cast<unsigned>(state->cpu.de),
                static_cast<unsigned>(state->cpu.hl),
                msx_cpu_run_state_label(state->cpu.runState),
                irqEnabled ? 1u : 0u,
                state->cpu.irqPending ? 1u : 0u,
                state->cpu.iff1 ? 1u : 0u,
                static_cast<unsigned long>(executedCycles),
                vdpSliceMode ? 1u : 0u,
                static_cast<unsigned>(state->memory.slotRegister),
                static_cast<unsigned>(msx_core_slot_for_page(state->memory.slotRegister, 0u)),
                static_cast<unsigned>(msx_core_slot_for_page(state->memory.slotRegister, 1u)),
                static_cast<unsigned>(msx_core_slot_for_page(state->memory.slotRegister, 2u)),
                static_cast<unsigned>(msx_core_slot_for_page(state->memory.slotRegister, 3u)),
                static_cast<unsigned>(state->memory.secondarySlotRegs[3]),
                static_cast<unsigned>(cart.windowBanks[0]),
                static_cast<unsigned>(cart.windowBanks[1]),
                static_cast<unsigned>(cart.windowBanks[2]),
                static_cast<unsigned>(cart.windowBanks[3]),
                msx_vdp_mode_label(vdp.mode),
                static_cast<unsigned>(vdp.activeHeight),
                (vdp.regs[1] & 0x40u) != 0u ? 1u : 0u,
                static_cast<unsigned>(vdp.regs[0]),
                static_cast<unsigned>(vdp.regs[1]),
                static_cast<unsigned>(vdp.regs[2]),
                static_cast<unsigned>(vdp.regs[7]),
                static_cast<unsigned>(vdp.regs[9]),
                static_cast<unsigned>(vdp.regs[18]),
                static_cast<unsigned>(vdp.regs[23]),
                static_cast<unsigned>(vdp.regs[25]),
                static_cast<unsigned>(vdp.regs[26]),
                static_cast<unsigned>(vdp.regs[27]),
                static_cast<unsigned>(vdp.status[0]),
                static_cast<unsigned>(vdp.status[1]),
                static_cast<unsigned>(vdp.status[2]));
}
#else
void msx_core_log_bootstrap_frame(const MsxCoreState*, bool, uint32_t, bool) {}
#endif

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

    // Standard "AB" cartridge headers boot through the BIOS. The BIOS slot
    // routines select the cartridge page when probing/calling its INIT vector.
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
    if (state->audioHookReady && state->scc.ready) {
        msx_memory_attach_scc(&state->memory, &state->scc);
        msx_scc_reset(&state->scc);
        const MsxVirtualSccMode sccMode = msx_config_get_virtual_scc_mode();
        msx_memory_set_virtual_scc_mode(&state->memory, sccMode);
        const bool sccOutput = (sccMode != MsxVirtualSccMode::Off) ||
                               (state->cart.type == MsxCartridgeType::KonamiScc);
        msx_scc_set_output_enabled(&state->scc, sccOutput);
    } else {
        msx_memory_attach_scc(&state->memory, nullptr);
    }
}

uint8_t msx_core_ram_segment_for_page(const MsxMemoryState* memory, uint8_t pageIndex)
{
    if (!memory || memory->ramSegmentCount == 0u) {
        return 0u;
    }

    if (memory->mapperEnabled && memory->ramSegmentCount > 4u) {
        return msx_memory_wrap_ram_segment(memory, memory->mapperRegisters[pageIndex & 0x03u]);
    }

    return msx_memory_wrap_ram_segment(memory, pageIndex);
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
    const bool slot3Expanded = memory->slot3Expanded;
    const uint8_t mainRamSlotId = slot3Expanded ? kMsxSlotIdMainRamExpanded
                                                : kMsxSlotIdMainRamPrimary;
    const uint8_t exptbl3 = slot3Expanded ? 0x80u : 0x00u;
    const uint8_t slttbl3 = slot3Expanded ? secondarySlotReg : 0x00u;

    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 0u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 1u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 2u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrExptbl + 3u), exptbl3);

    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 0u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 1u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 2u), 0x00u);
    msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrSlttbl + 3u), slttbl3);

    for (uint16_t i = 0u; i < 4u; ++i) {
        msx_core_raw_page3_write8(memory, static_cast<uint16_t>(kMsxAddrRamAd0 + i), mainRamSlotId);
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

    memory->cartBootWorkareaFallbackArmed = false;
    memory->cartBootMappingRestoreArmed = hasCartInit;

    MSX_CORE_LOG("[MSX] slot workarea: EXPTBL=%02X/%02X/%02X/%02X SLTTBL=%02X/%02X/%02X/%02X RAMAD=%02X MASTER=%02X CART=%02X INIT=%04X SLOT=%02X\n",
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(exptbl3),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(0x00u),
                 static_cast<unsigned>(slttbl3),
                 static_cast<unsigned>(mainRamSlotId),
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

bool msx_core_try_skip_boot_animation_internal(MsxCoreState* state)
{
    if (!state || !state->initialized) {
        return false;
    }
    if (state->directBoot) {
        return false;
    }
    if (!state->memory.cart.directBootCandidate || state->memory.cart.initAddress == 0u) {
        return false;
    }

    state->bootPc = state->memory.cart.initAddress;
    state->directBoot = true;
    msx_core_apply_boot_mapping(state,
                                kMsxBootSlotCart,
                                msx_core_default_secondary_slot_reg(state->machineMode, true));
    msx_cpu_init(&state->cpu);
    msx_cpu_reset(&state->cpu, state->bootPc, kMsxDefaultStack);
    state->vdp.dirty = true;
    state->frameCounter = 0u;
    state->lastStatusFrame = 0u;
    msx_core_set_status(state, "CART direct %04X", state->bootPc);
    msx_core_capture_status_state(state);

    std::printf("[MSX] boot skip: direct cart start pc=%04X\n",
                static_cast<unsigned>(state->bootPc));
    return true;
}

void msx_core_finish_no_cart_init(MsxCoreState* state)
{
    msx_core_attach_runtime_devices(state);
    const bool hasDiskRom = state &&
                            (state->memory.diskRom != nullptr) &&
                            (state->memory.diskRomSize != 0u);
    msx_core_apply_boot_mapping(state,
                                kMsxBootSlotBios,
                                hasDiskRom
                                    ? kMsxBootSecondaryDisk
                                    : msx_core_default_secondary_slot_reg(state->machineMode, false));

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
        if (state->cart.type == MsxCartridgeType::KonamiScc ||
            msx_config_get_virtual_scc_mode() != MsxVirtualSccMode::Off) {
            (void)msx_scc_init(&state->scc, audioSampleRate);
        }
        state->audioHookReady = true;
        return;
    }

    std::printf("[MSX] core audio: PSG init failed, continuing without sound\n");
}

} // namespace

bool msx_core_try_skip_boot_animation(MsxCoreState* state)
{
    return msx_core_try_skip_boot_animation_internal(state);
}

void msx_core_reset(MsxCoreState* state)
{
    if (!state || !state->initialized) {
        return;
    }

    state->initialized = false;
    state->frameCounter = 0u;
    state->lastFrameCpuUs = 0u;
    state->lastFrameVdpUs = 0u;
    state->lastFramePresentUs = 0u;
    state->lastFrameOtherUs = 0u;
    state->lastFrameTotalUs = 0u;
    state->lastFrameCycles = 0u;
    state->lastStatusFrame = 0u;
    state->lastInputJoy = 0xFFu;
    state->lastInputConfigFlags = 0u;
    state->lastInputJoystickMode = false;
    state->lastInputCaptured = false;
    msx_keyboard_matrix_clear(&state->lastInputKeyboardMatrix);

    if (state->disk.ready) {
        msx_disk_reset(&state->disk);
    }
    if (state->cas.ready) {
        state->cas.casPos = 0u;
    }

    msx_vdp_reset(&state->vdp);
    msx_memory_reset(&state->memory);
    msx_core_attach_runtime_devices(state);

    if (state->memory.cart.ready && state->memory.cart.rom && state->memory.cart.size != 0u) {
        state->bootPc = msx_core_select_boot_pc(&state->memory.cart, &state->directBoot);
        msx_core_apply_boot_mapping(
            state,
            state->directBoot ? kMsxBootSlotCart : kMsxBootSlotBios,
            msx_core_default_secondary_slot_reg(state->machineMode, state->directBoot)
        );
    } else {
        const bool hasDiskRom = (state->memory.diskRom != nullptr) &&
                                (state->memory.diskRomSize != 0u);
        state->bootPc = 0x0000u;
        state->directBoot = false;
        msx_core_apply_boot_mapping(
            state,
            kMsxBootSlotBios,
            hasDiskRom
                ? kMsxBootSecondaryDisk
                : msx_core_default_secondary_slot_reg(state->machineMode, false)
        );
    }

    msx_cpu_init(&state->cpu);
    msx_cpu_reset(&state->cpu, state->bootPc, kMsxDefaultStack);
    msx_vdp_render(&state->vdp);
    msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);

    state->initialized = true;
    msx_core_set_status(state,
                        "%s RESET %s",
                        state->memory.cart.ready ? "CART" : (state->disk.ready ? "DISK" : (state->cas.ready ? "CAS" : "BASIC")),
                        msx_media_bios_target_label(state->biosTarget));
    msx_core_capture_status_state(state);

    std::printf("[MSX] core reset: boot=%s pc=%04X machine=%s\n",
                state->directBoot ? "cart" : "bios",
                static_cast<unsigned>(state->bootPc),
                msx_config_machine_mode_label(state->machineMode));
}

bool msx_core_init(MsxCoreState* state,
                   const MsxRomImage* rom,
                   const MsxBiosBundle* bios,
                   const char* romName,
                   uint32_t audioSampleRate,
                   const uint8_t* cartSramData,
                   size_t cartSramSize)
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

    if (cartSramData && cartSramSize > 0u) {
        if (msx_cart_load_sram(&state->cart, cartSramData, cartSramSize)) {
            std::printf("[MSX] core init: cart SRAM loaded before memory map size=%u\n",
                        static_cast<unsigned>(cartSramSize));
        } else {
            std::printf("[MSX] core init: cart SRAM preload rejected size=%u\n",
                        static_cast<unsigned>(cartSramSize));
        }
    } else if (msx_cart_prepare_sram(&state->cart) && state->cart.sram) {
        std::printf("[MSX] core init: cart SRAM prepared before memory map size=%u\n",
                    static_cast<unsigned>(state->cart.sramSize));
    }

    std::printf("[MSX] core init: cart ok\n");
#if MSX_BOOTSTRAP_LOG_ENABLED
    if (msx_log_category_enabled(MsxLogCategory::Bootstrap)) {
        std::printf("[MSX][BOOTDBG] cart header off=%u init=%04X entry=%04X type=%s size=%u banks=%u direct=%u "
                    "head=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                    static_cast<unsigned>(state->cart.headerOffset),
                    static_cast<unsigned>(state->cart.initAddress),
                    static_cast<unsigned>(state->cart.entryPoint),
                    msx_media_cartridge_type_label(state->cart.type),
                    static_cast<unsigned>(state->cart.size),
                    static_cast<unsigned>(state->cart.bankCount8K),
                    state->cart.directBootCandidate ? 1u : 0u,
                    state->cart.rom && state->cart.size > 0u ? state->cart.rom[0] : 0xFFu,
                    state->cart.rom && state->cart.size > 1u ? state->cart.rom[1] : 0xFFu,
                    state->cart.rom && state->cart.size > 2u ? state->cart.rom[2] : 0xFFu,
                    state->cart.rom && state->cart.size > 3u ? state->cart.rom[3] : 0xFFu,
                    state->cart.rom && state->cart.size > 4u ? state->cart.rom[4] : 0xFFu,
                    state->cart.rom && state->cart.size > 5u ? state->cart.rom[5] : 0xFFu,
                    state->cart.rom && state->cart.size > 6u ? state->cart.rom[6] : 0xFFu,
                    state->cart.rom && state->cart.size > 7u ? state->cart.rom[7] : 0xFFu);
    }
#endif
    std::printf("[MSX] core init: vdp begin\n");
    if (!msx_vdp_init(&state->vdp, state->machineMode)) {
        std::printf("[MSX] core init failed at vdp init\n");
        msx_cart_shutdown(&state->cart);
        msx_bios_shutdown(&state->bios);
        return false;
    }
    if (state->machineMode == MsxMachineMode::MSX2) {
        if (state->vdp.vramSize < kMsx2VramSize) {
            std::printf("[MSX] core init failed: MSX2 VDP only got %u bytes VRAM\n",
                        static_cast<unsigned>(state->vdp.vramSize));
            msx_vdp_shutdown(&state->vdp);
            msx_cart_shutdown(&state->cart);
            msx_bios_shutdown(&state->bios);
            return false;
        }
        if (!msx_video_prepare_msx2_stream_buffers(512u, 212u)) {
            std::printf("[MSX] core init failed: MSX2 worst-case line-stream prealloc failed\n");
            msx_vdp_shutdown(&state->vdp);
            msx_cart_shutdown(&state->cart);
            msx_bios_shutdown(&state->bios);
            return false;
        }
    }
    std::printf("[MSX] core init: vdp ok vram=%u\n",
                static_cast<unsigned>(state->vdp.vramSize));

    const size_t requestedRamSize =
        (state->machineMode == MsxMachineMode::MSX1) ? kMsxCartRamSizeMsx1
                                                     : msx_core_select_msx2_ram_size(state->bios.mainRom, kMsxCoreInitReserve);
    std::printf("[MSX] core init: memory begin\n");
    std::printf("[MSX] core init: ram budget free=%u largest=%u requested=%u\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(requestedRamSize));
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, requestedRamSize)) {
        if (state->machineMode == MsxMachineMode::MSX2 && requestedRamSize > kMsxRamSizeMsx1) {
            std::printf("[MSX] core init: memory %uK failed after VDP-first, retrying 64K\n",
                        static_cast<unsigned>(requestedRamSize / 1024u));
            if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, kMsxRamSizeMsx1)) {
                std::printf("[MSX] core init failed at memory init\n");
                msx_vdp_shutdown(&state->vdp);
                msx_cart_shutdown(&state->cart);
                msx_bios_shutdown(&state->bios);
                return false;
            }
        } else {
            std::printf("[MSX] core init failed at memory init\n");
            msx_vdp_shutdown(&state->vdp);
            msx_cart_shutdown(&state->cart);
            msx_bios_shutdown(&state->bios);
            return false;
        }
    }

    std::printf("[MSX] core init: memory ok\n");

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
                 state->audioHookReady
                     ? ((state->scc.ready &&
                         msx_config_get_virtual_scc_mode() != MsxVirtualSccMode::Off)
                            ? "psg+scc"
                            : "psg")
                     : "off");
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

        if (msx_log_category_enabled(MsxLogCategory::CoreTrace)) {
            MSX_RUNTIME_LOG("[MSX][INPUT] cfg=%s joy=%02X psg=%02X/%02X keys=%s rows=%s #%u\n",
                            config,
                            static_cast<unsigned>(joy),
                            static_cast<unsigned>(psgPortA),
                            static_cast<unsigned>(psgPortB),
                            pressed,
                            rowSummary,
                            static_cast<unsigned>(s_inputLogCount));
            ++s_inputLogCount;
        }
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

    V9938_BENCH_FRAME_START(state->romName);

    const bool collectTiming = msx_log_category_enabled(MsxLogCategory::Profile);
    const int64_t frameStartUs = collectTiming ? esp_timer_get_time() : 0;
    uint32_t cpuRunUs = 0u;
    uint32_t vdpRenderUs = 0u;
    uint32_t presentUs = 0u;
    if (collectTiming) {
        msx_video_clear_last_present_us();
    }

    const bool vdpSliceMode =
                              state->machineMode == MsxMachineMode::MSX2 &&
                              state->vdp.mode != MsxVdpMode::Unsupported;
    const bool vdpSliceRenderMode =
                              vdpSliceMode &&
                              !msx_config_get_performance_flag(MsxPerformanceFlag::DisableSliceRendering) &&
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
        const unsigned totalLines = kMsxTotalScanlines60Hz;
        const unsigned vblankLine = visibleLines > 192u ? 230u : 220u;
        uint32_t executedCycles = 0u;
        int64_t vdpStartUs = collectTiming ? esp_timer_get_time() : 0;
        if (vdpSliceRenderMode) {
            msx_vdp_prepare_frame_render(&state->vdp);
        }
        if (collectTiming && vdpSliceRenderMode) {
            vdpRenderUs += static_cast<uint32_t>(esp_timer_get_time() - vdpStartUs);
        }

        bool msx2LineStream = false;
        if (vdpSliceRenderMode && state->machineMode == MsxMachineMode::MSX2 && !state->vdp.frameBuffer) {
            msx2LineStream = msx_vdp_begin_msx2_stream_frame(&state->vdp);
            if (!msx2LineStream) {
                MSX_CORE_LOG("[MSX] ERROR: msx_vdp_begin_msx2_stream_frame failed, video output may be lost!\n");
            }
        }

        state->vdp.status[0] &= static_cast<uint8_t>(~0x80u);
        state->vdp.status[1] &= static_cast<uint8_t>(~0x01u);
        state->vdp.status[2] &= static_cast<uint8_t>(~0x60u);
        for (unsigned line = 0; line < totalLines; ++line) {
            state->vdp.currentFrameCpuCycles = executedCycles;
            const uint32_t targetCycles = kMsxScanlineTargetCycles60Hz[line];
            const int sliceBudget = targetCycles > executedCycles
                                        ? static_cast<int>(targetCycles - executedCycles)
                                        : 0;
            const int64_t cpuStartUs = collectTiming ? esp_timer_get_time() : 0;
            executedCycles += static_cast<uint32_t>(msx_cpu_run_cycles(&state->cpu, &state->memory, sliceBudget));
            if (collectTiming) {
                cpuRunUs += static_cast<uint32_t>(esp_timer_get_time() - cpuStartUs);
            }
            state->vdp.currentFrameCpuCycles = executedCycles;
            msx_vdp_advance_command_engine(&state->vdp, executedCycles);
            msx_vdp_refresh_timing(&state->vdp);
            if (line + 1u == vblankLine) {
                state->vdp.status[0] |= 0x80u;
            }
            bool irqActive = false;
            if (((state->vdp.status[1] & 0x01u) != 0u) && ((state->vdp.regs[0] & 0x10u) != 0u)) {
                irqActive = true;
            }
            if (((state->vdp.status[0] & 0x80u) != 0u) && ((state->vdp.regs[1] & 0x20u) != 0u)) {
                irqActive = true;
            }
            if (irqActive) {
                msx_cpu_request_irq(&state->cpu);
            }
            if (vdpSliceRenderMode && line < visibleLines) {
                if (collectTiming) {
                    vdpStartUs = esp_timer_get_time();
                }
                state->vdp.sliceRenderCycles = executedCycles;
                msx_vdp_render_slice(&state->vdp, line, line + 1u, line + 1u == visibleLines);
                if (collectTiming) {
                    vdpRenderUs += static_cast<uint32_t>(esp_timer_get_time() - vdpStartUs);
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
        if (vdpSliceRenderMode) {
            state->vdp.dirty = false;
            state->vdp.frameReady = true;
        }

        if (msx2LineStream) {
            msx_vdp_end_msx2_stream_frame();
        }

        if (vdpSliceRenderMode) {
            if (collectTiming) {
                vdpStartUs = esp_timer_get_time();
            }
            msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);
            if (collectTiming) {
                vdpRenderUs += static_cast<uint32_t>(esp_timer_get_time() - vdpStartUs);
            }
            if (state->displayFrame.indexed8) {
                const int64_t presentStartUs = collectTiming ? esp_timer_get_time() : 0;
                msx_video_present_frame(&state->displayFrame);
                if (collectTiming) {
                    presentUs += static_cast<uint32_t>(esp_timer_get_time() - presentStartUs);
                }
            }
        }
    } else {
        const int64_t cpuStartUs = collectTiming ? esp_timer_get_time() : 0;
        state->lastFrameCycles = static_cast<uint32_t>(msx_cpu_run_cycles(&state->cpu, &state->memory, kMsxFrameCycles60Hz));
        if (collectTiming) {
            cpuRunUs += static_cast<uint32_t>(esp_timer_get_time() - cpuStartUs);
        }
    }

    msx_cpu_flush_pending_psg(&state->memory);

    // Log the first time the CPU enters a non-running state, and every 60 frames while stuck.
    const MsxCpuRunState curRunState = state->cpu.runState;
    if (curRunState != MsxCpuRunState::Running) {
        if (msx_log_category_enabled(MsxLogCategory::CoreTrace) &&
            (prevRunState != curRunState || (state->frameCounter % 60u) == 0u)) {
            MSX_RUNTIME_LOG("[MSX][CPU] state=%s pc=%04X op=%02X frame=%lu cycles=%lu\n",
                        msx_cpu_run_state_label(curRunState),
                        static_cast<unsigned>(state->cpu.pc),
                        static_cast<unsigned>(state->cpu.lastOpcode),
                        static_cast<unsigned long>(state->frameCounter),
                        static_cast<unsigned long>(state->lastFrameCycles));
        }
    } else if (msx_log_category_enabled(MsxLogCategory::CoreTrace) &&
               (state->frameCounter < 5u || (state->frameCounter % 60u) == 0u)) {
        MSX_RUNTIME_LOG("[MSX][CPU] RUNNING pc=%04X vdp=%s irq=%d frame=%lu\n",
                    static_cast<unsigned>(state->cpu.pc),
                    msx_vdp_mode_label(state->vdp.mode),
                    static_cast<int>(irqEnabled),
                    static_cast<unsigned long>(state->frameCounter));
    }

    if (msx_log_category_enabled(MsxLogCategory::CoreTrace) &&
        (state->frameCounter == 5u || state->frameCounter == 60u || state->frameCounter == 120u)) {
        MSX_RUNTIME_LOG("[MSX][VDP] DUMP frame=%lu slot=0x%02X  R0=%02X R1=%02X R2=%02X R3=%02X R4=%02X R5=%02X R6=%02X R7=%02X\n",
                    static_cast<unsigned long>(state->frameCounter),
                    static_cast<unsigned>(state->memory.slotRegister),
                    state->vdp.regs[0], state->vdp.regs[1],
                    state->vdp.regs[2], state->vdp.regs[3],
                    state->vdp.regs[4], state->vdp.regs[5],
                    state->vdp.regs[6], state->vdp.regs[7]);
    }

    if (!vdpSliceRenderMode) {
        const int64_t vdpStartUs = collectTiming ? esp_timer_get_time() : 0;
        msx_vdp_render(&state->vdp);
        msx_vdp_get_display_frame(&state->vdp, &state->displayFrame);
        if (collectTiming) {
            vdpRenderUs += static_cast<uint32_t>(esp_timer_get_time() - vdpStartUs);
            const uint32_t implicitPresentUs = msx_video_get_last_present_us();
            if (implicitPresentUs <= vdpRenderUs) {
                vdpRenderUs -= implicitPresentUs;
                presentUs += implicitPresentUs;
            }
        }
    }
    const uint32_t frameUs =
        collectTiming ? static_cast<uint32_t>(esp_timer_get_time() - frameStartUs) : 0u;
    const uint32_t accountedUs = cpuRunUs + vdpRenderUs + presentUs;
    state->lastFrameCpuUs = collectTiming ? cpuRunUs : 0u;
    state->lastFrameVdpUs = collectTiming ? vdpRenderUs : 0u;
    state->lastFramePresentUs = collectTiming ? presentUs : 0u;
    state->lastFrameOtherUs =
        collectTiming ? (frameUs > accountedUs ? (frameUs - accountedUs) : 0u) : 0u;
    state->lastFrameTotalUs = collectTiming ? frameUs : 0u;
    if (collectTiming) {
        msx_core_log_profile(state, frameUs, cpuRunUs, vdpRenderUs, presentUs, vdpSliceMode);
    }
    V9938_BENCH_FRAME_END();
    msx_core_log_bootstrap_frame(state, vdpSliceMode, state->lastFrameCycles, irqEnabled);
    state->frameCounter++;

    if (msx_core_status_needs_refresh(state)) {
        msx_core_refresh_status(state);
    }
}

void msx_core_set_virtual_scc_mode(MsxCoreState* state, MsxVirtualSccMode mode)
{
    if (!state || !state->initialized) {
        return;
    }

    if (!state->audioHookReady || state->audioSampleRate == 0u) {
        return;
    }

    if (mode != MsxVirtualSccMode::Off && !state->scc.ready) {
        if (msx_scc_init(&state->scc, state->audioSampleRate)) {
            msx_memory_attach_scc(&state->memory, &state->scc);
            msx_scc_reset(&state->scc);
        }
    }

    if (!state->scc.ready) {
        return;
    }

    msx_cpu_flush_pending_psg(&state->memory);
    msx_memory_set_virtual_scc_mode(&state->memory, mode);
    const bool sccOutput = (mode != MsxVirtualSccMode::Off) ||
                           (state->cart.type == MsxCartridgeType::KonamiScc);
    msx_scc_set_output_enabled(&state->scc, sccOutput);

    if (mode == MsxVirtualSccMode::Off) {
        if (state->cart.type != MsxCartridgeType::KonamiScc) {
            const size_t sccAvailable = msx_scc_available_samples(&state->scc);
            msx_scc_discard_samples(&state->scc, sccAvailable);
            msx_memory_attach_scc(&state->memory, nullptr);
            msx_scc_shutdown(&state->scc);
        }
    }
}

void msx_core_set_scc_hardware_detect(MsxCoreState* state, bool enabled)
{
    (void)enabled;
    if (!state || !state->initialized) {
        return;
    }

    msx_cpu_flush_pending_psg(&state->memory);
    msx_memory_refresh_scc_hardware_detect(&state->memory);
}

void msx_core_set_region_profile(MsxCoreState* state, MsxRegionProfile profile)
{
    if (!state || !state->initialized) {
        return;
    }

    state->regionProfile = profile;
    msx_memory_set_region_profile(&state->memory, profile);
}

size_t msx_core_drain_audio(MsxCoreState* state, int16_t* dst, size_t capacity)
{
    if (!state || !state->initialized || !state->audioHookReady) {
        return 0u;
    }

    msx_cpu_flush_pending_psg(&state->memory);

    if (!dst || capacity == 0u) {
        const size_t available = msx_psg_available_samples(&state->psg);
        msx_psg_discard_samples(&state->psg, available);
        const size_t sccAvailable = msx_scc_available_samples(&state->scc);
        msx_scc_discard_samples(&state->scc, sccAvailable);
        state->lastAudioSamples = 0u;
        return 0u;
    }

    const size_t sampleCount = msx_psg_read_samples(&state->psg, dst, capacity);
    uint16_t psgPeak = 0u;
    if (sampleCount != 0u && msx_log_category_enabled(MsxLogCategory::PsgPeak)) {
        for (size_t i = 0; i < sampleCount; ++i) {
            const int32_t sample = static_cast<int32_t>(dst[i]);
            const uint32_t magnitude = sample < 0 ? static_cast<uint32_t>(-sample) : static_cast<uint32_t>(sample);
            if (magnitude > psgPeak) {
                psgPeak = static_cast<uint16_t>(magnitude > 32768u ? 32768u : magnitude);
            }
        }
    }
    if (sampleCount != 0u && state->scc.ready) {
        int16_t sccBuffer[512];
        size_t samplesLeft = sampleCount;
        size_t offset = 0;
        
        while (samplesLeft > 0) {
            const size_t chunk = samplesLeft < 512u ? samplesLeft : 512u;
            const size_t sccCount = msx_scc_read_samples(&state->scc, sccBuffer, chunk);
            for (size_t i = 0; i < sccCount; ++i) {
                int32_t mixed = static_cast<int32_t>(dst[offset + i]) + static_cast<int32_t>(sccBuffer[i]);
                if (mixed > 32767) {
                    mixed = 32767;
                } else if (mixed < -32768) {
                    mixed = -32768;
                }
                dst[offset + i] = static_cast<int16_t>(mixed);
            }
            if (sccCount < chunk) {
                break;
            }
            offset += chunk;
            samplesLeft -= chunk;
        }
    }
    if (msx_log_category_enabled(MsxLogCategory::PsgPeak)) {
        static uint32_t s_lastPsgPeakLogFrame = 0u;
        static uint16_t s_lastPsgPeak = 0xFFFFu;
        static uint8_t s_lastPsgR7 = 0xFFu;
        static uint8_t s_lastPsgR8 = 0xFFu;
        static uint8_t s_lastPsgR9 = 0xFFu;
        static uint8_t s_lastPsgR10 = 0xFFu;

        const uint8_t r7 = state->psg.regs[7];
        const uint8_t r8 = state->psg.regs[8];
        const uint8_t r9 = state->psg.regs[9];
        const uint8_t r10 = state->psg.regs[10];
        const bool changed =
            psgPeak != s_lastPsgPeak ||
            r7 != s_lastPsgR7 ||
            r8 != s_lastPsgR8 ||
            r9 != s_lastPsgR9 ||
            r10 != s_lastPsgR10;
        const bool periodic = (state->frameCounter - s_lastPsgPeakLogFrame) >= 30u;
        if (changed || periodic) {
            MSX_CATEGORY_LOG(MsxLogCategory::PsgPeak,
                             "[MSX][PSG-PEAK] frame=%u n=%u peak=%u R7=%02X R8=%02X R9=%02X R10=%02X\n",
                             static_cast<unsigned>(state->frameCounter),
                             static_cast<unsigned>(sampleCount),
                             static_cast<unsigned>(psgPeak),
                             static_cast<unsigned>(r7),
                             static_cast<unsigned>(r8),
                             static_cast<unsigned>(r9),
                             static_cast<unsigned>(r10));
            s_lastPsgPeakLogFrame = state->frameCounter;
            s_lastPsgPeak = psgPeak;
            s_lastPsgR7 = r7;
            s_lastPsgR8 = r8;
            s_lastPsgR9 = r9;
            s_lastPsgR10 = r10;
        }
    }
    state->lastAudioSamples = static_cast<uint16_t>(sampleCount);
    return sampleCount;
}

void msx_core_shutdown(MsxCoreState* state)
{
    if (!state) {
        return;
    }

    msx_psg_shutdown(&state->psg);
    msx_scc_shutdown(&state->scc);
    msx_memory_shutdown(&state->memory);
    msx_vdp_shutdown(&state->vdp);
    msx_cart_shutdown(&state->cart);
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
    state->memory.slot3Expanded =
        (state->machineMode == MsxMachineMode::MSX2) ||
        ((diskRomData != nullptr) && (diskRomSize != 0u));
    if (state->memory.slot3Expanded && diskRomData && diskRomSize != 0u) {
        state->memory.secondarySlotRegs[3] = kMsxBootSecondaryDisk;
    }
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

    const size_t requestedRamSize = msx_core_select_system_ram_size(state->machineMode);
    std::printf("[MSX] core init_basic: memory begin\n");
    std::printf("[MSX] core init_basic: ram requested=%u\n",
                static_cast<unsigned>(requestedRamSize));
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, requestedRamSize)) {
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

    const size_t requestedRamSize = msx_core_select_system_ram_size(state->machineMode);
    std::printf("[MSX] core init_disk: memory begin\n");
    std::printf("[MSX] core init_disk: ram requested=%u\n",
                static_cast<unsigned>(requestedRamSize));
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, requestedRamSize)) {
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
    state->memory.slot3Expanded =
        (state->machineMode == MsxMachineMode::MSX2) ||
        ((diskRomData != nullptr) && (diskRomSize != 0u));
    if (state->memory.slot3Expanded && diskRomData && diskRomSize != 0u) {
        state->memory.secondarySlotRegs[3] = kMsxBootSecondaryDisk;
    }
    state->memory.diskPatch = msx_disk_bios_patch_handler;
    msx_disk_init(&state->disk, dskData, dskSize);
    state->memory.disk = &state->disk;
    std::printf("[MSX] core init_disk: diskRom=%s diskSize=%u inferredSides=%u\n",
                diskRomData ? "yes" : "no",
                static_cast<unsigned>(dskSize),
                static_cast<unsigned>(state->disk.sides));

    msx_core_finish_no_cart_init(state);
    if (diskRomData) {
        std::printf("[MSX] disk boot: using BIOS/DISK ROM entry path\n");
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

    const size_t requestedRamSize = msx_core_select_system_ram_size(state->machineMode);
    std::printf("[MSX] core init_cas: memory begin\n");
    std::printf("[MSX] core init_cas: ram requested=%u\n",
                static_cast<unsigned>(requestedRamSize));
    size_t currentRamTry = requestedRamSize;
    bool memoryOk = false;
    while (!memoryOk) {
        if (msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart, currentRamTry)) {
            memoryOk = true;
            break;
        }
        if (state->machineMode == MsxMachineMode::MSX2 && currentRamTry > kMsxRamSizeMsx1) {
            currentRamTry = (currentRamTry >= kMsxRamSizeMsx1 + 16384) ? (currentRamTry - 16384) : kMsxRamSizeMsx1;
            std::printf("[MSX] core init_cas: RAM alloc failed, falling back to %u bytes\n", static_cast<unsigned>(currentRamTry));
        } else {
            break;
        }
    }
    if (!memoryOk) {
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

bool msx_core_change_dsk(MsxCoreState* state,
                         const uint8_t* dskData, size_t dskSize,
                         const char* name)
{
    if (!state || !state->initialized) {
        std::printf("[MSX] core change_dsk failed: core not initialized\n");
        return false;
    }

    msx_disk_init(&state->disk, dskData, dskSize);
    state->memory.disk = &state->disk;
    std::snprintf(state->romName, sizeof(state->romName),
                  "%s", (name && name[0] != '\0') ? name : "MSX DISK");
    msx_core_set_status(state,
                        "DISK %s",
                        state->disk.ready ? "changed" : "not ready");
    std::printf("[MSX] core change_dsk: name=%s size=%u ready=%s sides=%u\n",
                state->romName,
                static_cast<unsigned>(dskSize),
                state->disk.ready ? "yes" : "no",
                static_cast<unsigned>(state->disk.sides));
    return state->disk.ready;
}

static void msx_core_fill_scc_save_payload(const MsxCoreState* state, MsxSccSavePayloadV1* payload)
{
    if (!state || !payload) {
        return;
    }

    std::memset(payload, 0, sizeof(*payload));
    std::memcpy(payload->regs, state->scc.regs, sizeof(payload->regs));
    std::memcpy(payload->phase, state->scc.phase, sizeof(payload->phase));
    std::memcpy(payload->step, state->scc.step, sizeof(payload->step));
    payload->sampleRate = state->scc.sampleRate;
    payload->cpuClockHz = state->scc.cpuClockHz;
    payload->sampleAccumulator = static_cast<uint64_t>(state->scc.sampleAccumulator);
    payload->generatedSamples = state->scc.generatedSamples;
    payload->droppedSamples = state->scc.droppedSamples;
    payload->audibleSamples = state->scc.audibleSamples;
    payload->lastAudibleLogSample = state->scc.lastAudibleLogSample;
    payload->audiblePeak = state->scc.audiblePeak;
    payload->audibleLogCount = state->scc.audibleLogCount;
    payload->classicWindow = state->scc.classicWindow ? 1u : 0u;
    payload->plusWindow = state->scc.plusWindow ? 1u : 0u;
    payload->sccPlusMode = state->scc.sccPlusMode ? 1u : 0u;
    payload->enabled = state->scc.enabled ? 1u : 0u;
    payload->outputEnabled = state->scc.outputEnabled ? 1u : 0u;
    payload->ready = state->scc.ready ? 1u : 0u;
    bool realClassic = false;
    bool realPlus = false;
    msx_memory_get_scc_window_state(&realClassic, &realPlus, nullptr);
    payload->realClassicWindow = realClassic ? 1u : 0u;
    payload->realPlusWindow = realPlus ? 1u : 0u;
}

static bool msx_core_write_scc_save_chunk(File& f, const MsxCoreState* state)
{
    MsxSccSavePayloadV1 payload = {};
    msx_core_fill_scc_save_payload(state, &payload);
    const MsxSaveChunkHeader header = {
        kMsxSaveChunkScc,
        kMsxSaveSccVersion,
        0u,
        static_cast<uint32_t>(sizeof(payload))
    };
    return f.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
           f.write(reinterpret_cast<const uint8_t*>(&payload), sizeof(payload)) == sizeof(payload);
}

static bool msx_core_read_scc_save_chunk(File& f, MsxSccSavePayloadV1* payload, bool* found)
{
    if (!payload || !found) {
        return false;
    }

    *found = false;
    while (f.available() >= static_cast<int>(sizeof(MsxSaveChunkHeader))) {
        MsxSaveChunkHeader header = {};
        if (f.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
            return false;
        }
        if (header.size == 0u || header.size > static_cast<uint32_t>(f.available())) {
            return false;
        }

        if (header.magic == kMsxSaveChunkScc &&
            header.version == kMsxSaveSccVersion &&
            header.size >= sizeof(MsxSccSavePayloadV1)) {
            if (f.read(reinterpret_cast<uint8_t*>(payload), sizeof(*payload)) != sizeof(*payload)) {
                return false;
            }
            const uint32_t extra = header.size - static_cast<uint32_t>(sizeof(*payload));
            if (extra != 0u) {
                f.seek(f.position() + extra);
            }
            *found = true;
            return true;
        }

        f.seek(f.position() + header.size);
    }
    return true;
}

static void msx_core_apply_scc_save_payload(MsxCoreState* state, const MsxSccSavePayloadV1& payload)
{
    if (!state) {
        return;
    }

    if (payload.ready == 0u) {
        if (state->scc.ready) {
            msx_scc_reset(&state->scc);
        }
        msx_memory_restore_scc_window_state(&state->memory,
                                            payload.realClassicWindow != 0u,
                                            payload.realPlusWindow != 0u,
                                            msx_config_get_virtual_scc_mode());
        return;
    }

    if (!state->scc.ready) {
        const uint32_t sampleRate = state->audioSampleRate != 0u
                                        ? state->audioSampleRate
                                        : payload.sampleRate;
        if (!msx_scc_init(&state->scc, sampleRate)) {
            std::printf("[MSX][STATE] SCC chunk ignored: init failed\n");
            return;
        }
    }

    int16_t* ringPtr = state->scc.ring;
    std::memcpy(state->scc.regs, payload.regs, sizeof(state->scc.regs));
    std::memcpy(state->scc.phase, payload.phase, sizeof(state->scc.phase));
    std::memcpy(state->scc.step, payload.step, sizeof(state->scc.step));
    state->scc.sampleRate = state->audioSampleRate != 0u ? state->audioSampleRate : payload.sampleRate;
    state->scc.cpuClockHz = payload.cpuClockHz != 0u ? payload.cpuClockHz : state->scc.cpuClockHz;
    state->scc.sampleAccumulator =
        payload.sampleAccumulator > 0xFFFFFFFFull
            ? 0u
            : static_cast<uint32_t>(payload.sampleAccumulator);
    state->scc.ringReadIndex = 0u;
    state->scc.ringWriteIndex = 0u;
    state->scc.ringCount = 0u;
    state->scc.generatedSamples = payload.generatedSamples;
    state->scc.droppedSamples = payload.droppedSamples;
    state->scc.audibleSamples = payload.audibleSamples;
    state->scc.lastAudibleLogSample = payload.lastAudibleLogSample;
    state->scc.audiblePeak = payload.audiblePeak;
    state->scc.audibleLogCount = payload.audibleLogCount;
    state->scc.ring = ringPtr;
    state->scc.classicWindow = payload.classicWindow != 0u;
    state->scc.plusWindow = payload.plusWindow != 0u;
    state->scc.sccPlusMode = payload.sccPlusMode != 0u;
    state->scc.enabled = payload.enabled != 0u;
    state->scc.outputEnabled = payload.outputEnabled != 0u;
    state->scc.ready = true;

    msx_scc_recompute_steps(&state->scc);
    msx_memory_attach_scc(&state->memory, &state->scc);
    msx_memory_restore_scc_window_state(&state->memory,
                                        payload.realClassicWindow != 0u,
                                        payload.realPlusWindow != 0u,
                                        msx_config_get_virtual_scc_mode());
}

bool msx_core_save_state(MsxCoreState* state, const char* path)
{
    if (!state) return false;

    msx_cpu_flush_pending_psg(&state->memory);

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

    if (!msx_core_write_scc_save_chunk(f, state)) {
        std::printf("[MSX][STATE] Error: could not write SCC chunk\n");
        f.close();
        return false;
    }

    f.close();
    std::printf("[MSX][STATE] Save completed successfully\n");
    return true;
}

bool msx_core_load_state(MsxCoreState* state, const char* path)
{
    if (!state) return false;

    msx_cpu_clear_pending_psg();

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
    uint8_t* memoryCartSram = state->memory.cart.sram;
    const size_t memoryCartSramSize = state->memory.cart.sramSize;
    const bool memoryCartSramDirty = state->memory.cart.sramDirty;
    const bool memoryCartOwnsSram = state->memory.cart.ownsSram;
    uint8_t* cartSram = state->cart.sram;
    const size_t cartSramSize = state->cart.sramSize;
    const bool cartSramDirty = state->cart.sramDirty;
    const bool cartOwnsSram = state->cart.ownsSram;
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
    state->memory.cart.sram = memoryCartSram;
    state->memory.cart.sramSize = memoryCartSramSize;
    state->memory.cart.sramDirty = memoryCartSramDirty;
    state->memory.cart.ownsSram = memoryCartOwnsSram;
    state->cart.rom = cartRom;
    state->cart.sram = cartSram;
    state->cart.sramSize = cartSramSize;
    state->cart.sramDirty = cartSramDirty;
    state->cart.ownsSram = cartOwnsSram;
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

    MsxSccSavePayloadV1 sccPayload = {};
    bool sccChunkFound = false;
    if (!msx_core_read_scc_save_chunk(f, &sccPayload, &sccChunkFound)) {
        std::printf("[MSX][STATE] Warning: SCC chunk read failed\n");
    }

    f.close();

    if (sccChunkFound) {
        msx_core_apply_scc_save_payload(state, sccPayload);
    } else if (state->scc.ready) {
        msx_scc_reset(&state->scc);
        msx_memory_restore_scc_window_state(&state->memory,
                                            false,
                                            false,
                                            msx_config_get_virtual_scc_mode());
    }

    state->vdp.dirty = true;
    msx_memory_refresh_maps(&state->memory);
    std::printf("[MSX][STATE] Load completed successfully\n");
    return true;
}

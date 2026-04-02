Continue = 'Stop'

function Replace-Exact {
    param(
        [string],
        [string],
        [string]
    )
     = Get-Content -Path  -Raw
    if (-not .Contains()) {
        throw \"Pattern not found in \"
    }
     = .Replace(, )
    Set-Content -Path  -Value 
}

Replace-Exact -Path 'src\\main.cpp' -Old '#include "msx/run_msx.h"' -New "#include \"msx/run_msx.h\"
#include \"msx/msx_config.h\""

 = @'
static bool getProfileForRomType(RomType romType, share::EmuProfile& outProfile) {
  switch (romType) {
    case ROM_TYPE_A2600:
      outProfile = share::EmuProfile::A2600;
      return true;
    case ROM_TYPE_A7800:
      outProfile = share::EmuProfile::A7800;
      return true;
    case ROM_TYPE_MSX:
      outProfile = share::EmuProfile::MSX;
      return true;
    default:
      return false;
  }
}
'@

 = @'
static bool getProfileForRomType(RomType romType, share::EmuProfile& outProfile) {
  switch (romType) {
    case ROM_TYPE_A2600:
      outProfile = share::EmuProfile::A2600;
      return true;
    case ROM_TYPE_A7800:
      outProfile = share::EmuProfile::A7800;
      return true;
    case ROM_TYPE_MSX:
      outProfile = share::EmuProfile::MSX;
      return true;
    default:
      return false;
  }
}

static void selectMsxLaunchSystem(CardputerView& display, CardputerInput& input, RomType romType)
{
  if (romType != ROM_TYPE_MSX) {
    return;
  }

  const MsxMachineMode savedMode = msx_config_load_machine_mode();
  VerticalSelector selector(display, input);
  const std::vector<std::string> options = {
    "Auto (MSX1 first)",
    "MSX1",
    "MSX2",
  };

  int initialIndex = 0;
  switch (savedMode) {
    case MsxMachineMode::MSX1:
      initialIndex = 1;
      break;
    case MsxMachineMode::MSX2:
      initialIndex = 2;
      break;
    case MsxMachineMode::Auto:
    default:
      initialIndex = 0;
      break;
  }

  display.topBar("SELECT MSX SYSTEM", false, false);
  const int selected = selector.select("Launch system",
                                       options,
                                       false,
                                       false,
                                       {},
                                       {},
                                       false,
                                       true,
                                       true,
                                       initialIndex);

  int resolvedIndex = selected;
  if (resolvedIndex < 0) {
    resolvedIndex = initialIndex;
  }

  MsxMachineMode launchMode = MsxMachineMode::Auto;
  if (resolvedIndex == 1) {
    launchMode = MsxMachineMode::MSX1;
  } else if (resolvedIndex == 2) {
    launchMode = MsxMachineMode::MSX2;
  }

  msx_config_set_machine_mode(launchMode, false);
  printf("[MSX] launch mode selected: %s\n", msx_config_machine_mode_label(launchMode));
}
'@
Replace-Exact -Path 'src\\main.cpp' -Old  -New 
Replace-Exact -Path 'src\\main.cpp' -Old '  // Display target selection (for cores that support external TFT)' -New "  selectMsxLaunchSystem(display, input, ext);

  // Display target selection (for cores that support external TFT)"

 = @'
    if (msx_load_for_target(bundle, MsxBiosTarget::MSX2, config)) {
        return true;
    }

    return msx_load_for_target(bundle, MsxBiosTarget::MSX1, config);
'@
 = @'
    if (msx_load_for_target(bundle, MsxBiosTarget::MSX1, config)) {
        return true;
    }

    if (bundle->mainRom.status == MsxImageLoadStatus::Incompatible ||
        bundle->subRom.status == MsxImageLoadStatus::Incompatible) {
        return false;
    }

    return msx_load_for_target(bundle, MsxBiosTarget::MSX2, config);
'@
Replace-Exact -Path 'src\\msx\\msx_media.cpp' -Old  -New 

Replace-Exact -Path 'src\\msx\\run_msx.cpp' -Old '            introLines[4] = "AUTO tries MSX2 first";' -New '            introLines[4] = "AUTO tries MSX1 first";'
Replace-Exact -Path 'src\\msx\\run_msx.cpp' -Old '            introLines[5] = "Need: MSX2.ROM + MSX2EXT.ROM";' -New '            introLines[5] = "Fallback: MSX2.ROM + MSX2EXT.ROM";'
Replace-Exact -Path 'src\\msx\\run_msx.cpp' -Old '            introLines[6] = "Fallback: MSX.ROM";' -New '            introLines[6] = "Need: MSX.ROM";'

Replace-Exact -Path 'src\\msx\\core\\msx_core.cpp' -Old @'
    if (!state || !rom || !bios || !rom->data || rom->size == 0 || !rom->sizeSupported || !bios->compatible) {
        return false;
    }
'@ -New @'
    if (!state || !rom || !bios || !rom->data || rom->size == 0 || !rom->sizeSupported || !bios->compatible) {
        std::printf("[MSX] core init failed: invalid launch data\n");
        return false;
    }
'@
Replace-Exact -Path 'src\\msx\\core\\msx_core.cpp' -Old @'
    if (!msx_bios_init(&state->bios, bios)) {
        return false;
    }
'@ -New @'
    if (!msx_bios_init(&state->bios, bios)) {
        std::printf("[MSX] core init failed at bios init\n");
        return false;
    }
'@
Replace-Exact -Path 'src\\msx\\core\\msx_core.cpp' -Old @'
    if (!msx_cart_init(&state->cart, rom)) {
        msx_bios_shutdown(&state->bios);
        return false;
    }
'@ -New @'
    if (!msx_cart_init(&state->cart, rom)) {
        std::printf("[MSX] core init failed at cart init\n");
        msx_bios_shutdown(&state->bios);
        return false;
    }
'@
Replace-Exact -Path 'src\\msx\\core\\msx_core.cpp' -Old @'
    if (!msx_vdp_init(&state->vdp, state->machineMode)) {
        msx_bios_shutdown(&state->bios);
        std::memset(&state->cart, 0, sizeof(state->cart));
        return false;
    }
'@ -New @'
    if (!msx_vdp_init(&state->vdp, state->machineMode)) {
        std::printf("[MSX] core init failed at vdp init\n");
        msx_bios_shutdown(&state->bios);
        std::memset(&state->cart, 0, sizeof(state->cart));
        return false;
    }
'@
Replace-Exact -Path 'src\\msx\\core\\msx_core.cpp' -Old @'
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart)) {
        msx_vdp_shutdown(&state->vdp);
        msx_bios_shutdown(&state->bios);
        std::memset(&state->cart, 0, sizeof(state->cart));
        return false;
    }
'@ -New @'
    if (!msx_memory_init(&state->memory, state->machineMode, &state->bios, &state->cart)) {
        std::printf("[MSX] core init failed at memory init\n");
        msx_vdp_shutdown(&state->vdp);
        msx_bios_shutdown(&state->bios);
        std::memset(&state->cart, 0, sizeof(state->cart));
        return false;
    }
'@

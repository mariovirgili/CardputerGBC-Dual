#include "snes_stubs.h"
#include "compat/arduino_compat.h"
#include <cstdio>

extern "C" {
    #include "snes9x/snes9x.h"
#include "share/emu_log_cpp.h"
}

void S9xDeinitDisplay(void)
{
    // Nothing
}

void S9xExtraUsage()
{
    // Stub
}

void S9xExit()
{
    EMU_LOG("[SNES] S9xExit called\n");
}

void S9xMessage(int type, int number, const char* message)
{
    (void)type;
    (void)number;
    EMU_LOG("[SNES] %s\n", message ? message : "(null)");
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    (void)which1;
    (void)x;
    (void)y;
    (void)buttons;
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    (void)x;
    (void)y;
    (void)buttons;
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

#ifdef SNES_NO_SOUND

void S9xAPUWritePort(int32_t port, uint8_t value)
{
    const uint8_t p = (uint8_t)(port & 3);
    const uint16_t addr = (uint16_t)(0x2140u + p);

    // Mirror write like Snes9x impl
    Memory.FillRAM[addr] = value;
}

uint8_t S9xAPUReadPort(int32_t port)
{
    const uint8_t p = (uint8_t)(port & 3);
    CPU.BranchSkip = true;

    if (p == 0)
        return 0xAA;
    if (p == 1)
        return 0xBB;

    const uint16_t addr = (uint16_t)(0x2140u + p);
    return Memory.FillRAM[addr];
}

void S9xResetAPU()
{
    // Nothing
}

#endif

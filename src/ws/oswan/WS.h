/*
$Date: 2009-10-30 05:26:46 +0100 (ven., 30 oct. 2009) $
$Rev: 71 $
*/

#ifndef WS_H_
#define WS_H_

#include "WSHard.h"

struct EEPROM
{
    WORD *data;
    int we;
};

extern int Run;
extern BYTE *Page[0x10];
extern BYTE* IRAM;
extern BYTE *IO;
extern BYTE *MemDummy;
extern BYTE **ROMMap;     // C-ROM bank map
extern int ROMBanks;      // C-ROM bank count
extern BYTE **RAMMap;     // C-RAM bank map
extern int RAMBanks;      // C-RAM bank count
extern int RAMSize;       // C-RAM size
extern DWORD WsLastDMASrc;
extern WORD WsLastDMADst;
extern WORD WsLastDMACnt;
extern BYTE WsLastDMASrcBytes[4];
extern BYTE WsLastDMAValid;
extern WORD IEep[64];
extern struct EEPROM sIEep;
extern struct EEPROM sCEep;

#define CK_EEP 1
extern int CartKind;

void WriteIO(DWORD A, BYTE V);
void WsReset (void);
void WsRomPatch(BYTE *buf);
int WsRun(void);
void WsSplash(void);
void WsCpyPdata(BYTE* dst);
void Sleep(int);
void SetHVMode(int Mode);

void WsInit(void);
void WsDeInit(void);

#endif

//
// Copyright (c) 2004 K. Wilkins
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from
// the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//

//////////////////////////////////////////////////////////////////////////////
//                       Handy - An Atari Lynx Emulator                     //
//                          Copyright (c) 1996,1997                         //
//                                 K. Wilkins                               //
//////////////////////////////////////////////////////////////////////////////
// Lynx Cartridge Class                                                     //
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
// This class emulates the Lynx cartridge interface, given a filename it    //
// will contstruct a cartridge object via the constructor.                  //
//                                                                          //
//    K. Wilkins                                                            //
// August 1997                                                              //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////
// Revision History:                                                        //
// -----------------                                                        //
//                                                                          //
// 01Aug1997 KW Document header added & class documented.                   //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

//#define   TRACE_CART

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "cart.h"

#define CART_INC_COUNTER() {if(!mStrobe) mCounter = (mCounter + 1) & 0x07ff;}

// ---------------------------------------------------------------------------
//   CCart::CCart — version XIP (ESP32)
// ---------------------------------------------------------------------------
CCart::CCart(const UBYTE *gamedata, ULONG gamesize)
{
   TRACE_CART1("CCart(XIP) called with %s", gamefile);

   mWriteEnableBank1 = FALSE;
   mCartRAM          = FALSE;
   mCRC32            = 0;
   mBank             = bank0;

   // default header values
   mFileHeader = (LYNX_HEADER){
      .magic            = {0, 0, 0, 0},
      .page_size_bank0  = UWORD(gamesize >> 8),
      .page_size_bank1  = 0,
      .version          = 0,
      .cartname         = {'N','O',' ','H','E','A','D','E','R'},
      .manufname        = {'H','A','N','D','Y'},
      .rotation         = 0,
      .aud_bits         = 0,
      .eeprom           = 0,
      .spare            = {0, 0, 0},
   };

   const UBYTE* data = gamedata;
   ULONG size        = gamesize;

   // Read LNX header if present
   if (gamedata && gamesize > sizeof(LYNX_HEADER)) {
      if (memcmp(gamedata, "LYNX", 4) == 0) {
         memcpy(&mFileHeader, gamedata, sizeof(LYNX_HEADER));
         #ifdef MSB_FIRST
         mFileHeader.page_size_bank0 = ((mFileHeader.page_size_bank0>>8) | (mFileHeader.page_size_bank0<<8));
         mFileHeader.page_size_bank1 = ((mFileHeader.page_size_bank1>>8) | (mFileHeader.page_size_bank1<<8));
         mFileHeader.version         = ((mFileHeader.version>>8) | (mFileHeader.version<<8));
         #endif
         data += sizeof(LYNX_HEADER);
         size -= sizeof(LYNX_HEADER);
      } else {
         log_printf("No header found: %02X %02X %02X %02X.\n",
            gamedata[0], gamedata[1], gamedata[2], gamedata[3]);
      }
      mCRC32 = crc32_le(0, data, size);
      gCPUBootAddress = 0;
   }

   // Calculate Bank0 masks
   switch(mFileHeader.page_size_bank0) {
      case 0x000:
         mMaskBank0    = 0;
         mShiftCount0  = 0;
         mCountMask0   = 0;
         break;
      case 0x100:
         mMaskBank0    = 0x00ffff;
         mShiftCount0  = 8;
         mCountMask0   = 0x0ff;
         break;
      case 0x200:
         mMaskBank0    = 0x01ffff;
         mShiftCount0  = 9;
         mCountMask0   = 0x1ff;
         break;
      case 0x400:
         mMaskBank0    = 0x03ffff;
         mShiftCount0  = 10;
         mCountMask0   = 0x3ff;
         break;
      case 0x800:
         mMaskBank0    = 0x07ffff;
         mShiftCount0  = 11;
         mCountMask0   = 0x7ff;
         break;
      default:
         log_printf("Invalid bank0 size (0x%03X).\n", mFileHeader.page_size_bank0);
         mMaskBank0    = size ? (size - 1) : 0;
         mShiftCount0  = 0;
         mCountMask0   = 0;
         break;
   }
   TRACE_CART1("CCart(XIP) - Bank0 = $%06x", mMaskBank0);

   // Calculate Bank1 masks
   switch(mFileHeader.page_size_bank1) {
      case 0x000:
         // Bank1 empty -> SRAM/EEPROM shadow, treat as non-existent in XIP
         mMaskBank1      = 0;
         mShiftCount1    = 0;
         mCountMask1     = 0;
         mWriteEnableBank1 = TRUE;
         mCartRAM        = TRUE;
         break;
      case 0x100:
         mMaskBank1    = 0x00ffff;
         mShiftCount1  = 8;
         mCountMask1   = 0x0ff;
         break;
      case 0x200:
         mMaskBank1    = 0x01ffff;
         mShiftCount1  = 9;
         mCountMask1   = 0x1ff;
         break;
      case 0x400:
         mMaskBank1    = 0x03ffff;
         mShiftCount1  = 10;
         mCountMask1   = 0x3ff;
         break;
      case 0x800:
         mMaskBank1    = 0x07ffff;
         mShiftCount1  = 11;
         mCountMask1   = 0x7ff;
         break;
      default:
         log_printf("Invalid bank1 size (0x%03X).\n", mFileHeader.page_size_bank1);
         mMaskBank1    = 0;
         mShiftCount1  = 0;
         mCountMask1   = 0;
         break;
   }
   TRACE_CART1("CCart(XIP) - Bank1 = $%06x", mMaskBank1);

   // Layout in ROM 
   int cartsize  = (int)size;
   int bank0size = __min(cartsize, (int)(mMaskBank0 + 1));
   int bank1size = __min(cartsize, (int)(mMaskBank1 + 1));

   if (bank0size == 1) bank0size = 0;
   if (bank1size == 1) bank1size = 0;

   // Instead of allocating and copying, we POINT directly into the XIP ROM
   mCartBank0 = (bank0size > 0) ? const_cast<UBYTE*>(data) : nullptr;
   cartsize   = __max(0, cartsize - bank0size);

   mCartBank1 = (bank1size > 0)
      ? const_cast<UBYTE*>(data + bank0size)
      : nullptr;
   cartsize   = __max(0, cartsize - bank1size);

   // No audio A banks in XIP for now
   mCartBank0A = nullptr;
   mCartBank1A = nullptr;

   log_printf("Cart(XIP) name='%s', crc32=%08lX, bank0=%d, bank1=%d\n",
      mFileHeader.cartname, (unsigned long)mCRC32, bank0size, bank1size);
}

CCart::~CCart()
{
   TRACE_CART0("~CCart()");
   if (mCartBank0) delete[] mCartBank0;
   if (mCartBank1) delete[] mCartBank1;
   if (mCartBank0A) delete[] mCartBank0A;
   if (mCartBank1A) delete[] mCartBank1A;
}

void CCart::Reset(void)
{
   TRACE_CART0("Reset()");
   mCounter=0;
   mShifter=0;
   mAddrData=0;
   mStrobe=0;
}

bool CCart::ContextSave(LSS_FILE *fp)
{
   TRACE_CART0("ContextSave()");
   if(!lss_printf(fp,"CCart::ContextSave")) return 0;
   if(!lss_write(&mCounter,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mShifter,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mAddrData,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mStrobe,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mShiftCount0,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mCountMask0,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mShiftCount1,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mCountMask1,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mBank,sizeof(EMMODE),1,fp)) return 0;
   if(!lss_write(&mDummy,sizeof(ULONG),1,fp)) return 0;
   if(!lss_write(&mWriteEnableBank1,sizeof(ULONG),1,fp)) return 0;

   if(!lss_write(&mCartRAM,sizeof(ULONG),1,fp)) return 0;
   if(mCartRAM) {
      if(!lss_write(&mMaskBank1,sizeof(ULONG),1,fp)) return 0;
      if(!lss_write(mCartBank1,sizeof(UBYTE),mMaskBank1+1,fp)) return 0;
   }
   return 1;
}

bool CCart::ContextLoad(LSS_FILE *fp)
{
   TRACE_CART0("ContextLoad()");
   char teststr[32]="XXXXXXXXXXXXXXXXXX";
   if(!lss_read(teststr,sizeof(char),18,fp)) return 0;
   if(strcmp(teststr,"CCart::ContextSave")!=0) return 0;
   if(!lss_read(&mCounter,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mShifter,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mAddrData,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mStrobe,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mShiftCount0,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mCountMask0,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mShiftCount1,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mCountMask1,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mBank,sizeof(EMMODE),1,fp)) return 0;
   if(!lss_read(&mDummy,sizeof(ULONG),1,fp)) return 0;
   if(!lss_read(&mWriteEnableBank1,sizeof(ULONG),1,fp)) return 0;

   if(!lss_read(&mCartRAM,sizeof(ULONG),1,fp)) return 0;
   if(mCartRAM) {
      if(!lss_read(&mMaskBank1,sizeof(ULONG),1,fp)) return 0;
      delete[] mCartBank1;
      mCartBank1 = new UBYTE[mMaskBank1+1];
      if(!lss_read(mCartBank1,sizeof(UBYTE),mMaskBank1+1,fp)) return 0;
   }
   return 1;
}

void CCart::CartAddressStrobe(bool strobe)
{
   static int last_strobe=0;

   mStrobe=strobe;

   if(mStrobe) mCounter=0;

   //
   // Either of the two below seem to work OK.
   //
   // if(!strobe && last_strobe)
   //
   if(mStrobe && !last_strobe) {
      // Clock a bit into the shifter
      mShifter=mShifter<<1;
      mShifter+=mAddrData?1:0;
      mShifter&=0xff;
   }
   last_strobe=mStrobe;
   TRACE_CART2("CartAddressStrobe(strobe=%d) mShifter=$%06x",strobe,mShifter);
}

void CCart::CartAddressData(bool data)
{
   TRACE_CART1("CartAddressData($%02x)",data);
   mAddrData=data;
}

inline void CCart::Poke(ULONG addr, UBYTE data)
{
   if(mBank==bank1 && mWriteEnableBank1) {
      mCartBank1[addr&mMaskBank1]=data;
   }
}

inline UBYTE CCart::Peek(ULONG addr)
{
   if(mBank==bank0) {
      return(mCartBank0[addr&mMaskBank0]);
   } else {
      return(mCartBank1[addr&mMaskBank1]);
   }
}

void CCart::Poke0(UBYTE data)
{
   CART_INC_COUNTER();
}

void CCart::Poke0A(UBYTE data)
{
	CART_INC_COUNTER();
}

void CCart::Poke1(UBYTE data)
{
   if(mWriteEnableBank1) {
      ULONG address=(mShifter<<mShiftCount1)+(mCounter&mCountMask1);
      mCartBank1[address&mMaskBank1]=data;
   }
   CART_INC_COUNTER();
}

void CCart::Poke1A(UBYTE data)
{
	if(mWriteEnableBank1 && mCartBank1A) {
       ULONG address=(mShifter<<mShiftCount1)+(mCounter&mCountMask1);
       mCartBank1A[address&mMaskBank1]=data;
	}
	CART_INC_COUNTER();
}

UBYTE CCart::Peek0(void)
{
   ULONG address=(mShifter<<mShiftCount0)+(mCounter&mCountMask0);
   UBYTE data=mCartBank0[address&mMaskBank0];

   CART_INC_COUNTER();

   return data;
}

UBYTE CCart::Peek0A(void)
{
	ULONG address=(mShifter<<mShiftCount0)+(mCounter&mCountMask0);
	UBYTE data=mCartBank0A?mCartBank0A[address&mMaskBank0]:0xFF;

	CART_INC_COUNTER();

	return data;
}

UBYTE CCart::Peek1(void)
{
   ULONG address=(mShifter<<mShiftCount1)+(mCounter&mCountMask1);
   UBYTE data=mCartBank1[address&mMaskBank1];

   CART_INC_COUNTER();

   return data;
}

UBYTE CCart::Peek1A(void)
{
	ULONG address=(mShifter<<mShiftCount1)+(mCounter&mCountMask1);
	UBYTE data=mCartBank1A?mCartBank1A[address&mMaskBank1]:0xFF;

	CART_INC_COUNTER();

	return data;
}

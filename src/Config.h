#ifndef CONFIG_H
#define CONFIG_H

#include <TFT_eSPI.h> 

// ==========================================
// SD CARD PINS (M5Cardputer)
// ==========================================
static constexpr int SD_SCK_GPIO  = 40;
static constexpr int SD_MISO_GPIO = 39;
static constexpr int SD_MOSI_GPIO = 14;
static constexpr int SD_CS_GPIO   = 12;

// ==========================================
// General Configurations
// ==========================================
#define MAX_FILES 80

// Colors
#define BROWSER_COLOR_DIR     TFT_CYAN
#define BROWSER_COLOR_FILE    TFT_GREEN
#define BROWSER_COLOR_CURSOR  TFT_WHITE

#endif
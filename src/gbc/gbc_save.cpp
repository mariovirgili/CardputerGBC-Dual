#include "gbc_save.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/game_save.h"

// ==== API ====
extern "C" {
  bool gnuboy_sram_dirty(void);
  int  gnuboy_load_sram(const char *file);
  int  gnuboy_save_sram(const char *file, bool quick_save);
}

// ==== Config ====
#define GBC_SAVE_DIR "/sd/gbc_saves"

// ==== Etat ====
static char*        g_save_path   = nullptr;
static TaskHandle_t g_task        = nullptr;
static TickType_t   g_next_check  = 0;
static TickType_t   g_next_allow  = 0;
static TickType_t   g_first_dirty = 0;
static TickType_t   g_last_save   = 0;

static volatile bool g_flag_check = false;
static volatile bool g_flag_flush = false;

// ============================================================================
// Utils FS
// ============================================================================
static void make_save_path(const char* romPathOrName) {
  // Get the base filename (without full path) from the ROM path or name
  const char* base = share::gameSaveBasename(romPathOrName);
  char name[160] = {0};  // Temporary buffer for the save filename

  if (base && *base) {
    // Copy base name safely into local buffer
    strncpy(name, base, sizeof(name) - 1);

    // Find the last dot in the filename (file extension separator)
    char *dot = strrchr(name, '.');

    if (dot) {
      // If an extension exists, terminate the string at the dot
      // This removes the original extension (e.g., .gb, .gbc)
      *dot = '\0';
    }

    // Append ".sav" safely, ensuring no buffer overflow
    strncat(name, ".sav", sizeof(name) - strlen(name) - 1);

  } else {
    // Fallback name if no valid base name is found
    strcpy(name, "gbc_autosave.sav");
  }

  // Build the final save path: GBC_SAVE_DIR/<filename>.sav
  int n = snprintf(g_save_path, PATH_MAX, GBC_SAVE_DIR "/%s", name);

  // Ensure null-termination if truncated
  if (n < 0 || (size_t)n >= PATH_MAX) {
    g_save_path[PATH_MAX - 1] = '\0';
  }
}

// ============================================================================
// Save
// ============================================================================
static bool save_now() {
  if (!g_save_path) return false;
  if (!share::gameSaveEnsureParentReady(GBC_SAVE_DIR)) {
    printf("[GBC][SAVE] storage path not ready, skip save\n");
    return false;
  }

  share::setGameIsSaving(true);

  int rc = -1;
  const int max_attempts = 3;

  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    // Partial save of dirty sectors
    rc = gnuboy_save_sram(g_save_path, true);
    if (rc == 0) {
      // Success
      break;
    }

    printf("[GBC][SAVE] gnuboy_save_sram failed (rc=%d) for %s (attempt %d/%d)\n",
           rc, g_save_path, attempt, max_attempts);

    if (attempt < max_attempts) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }

  share::setGameIsSaving(false);

  g_last_save   = xTaskGetTickCount();
  g_first_dirty = 0;

  printf("[GBC][SAVE] SRAM saved to %s\n", g_save_path);
  return true;
}


// ============================================================================
// Task
// ============================================================================
static void SaveTask(void* /*arg*/) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    bool do_check = g_flag_check; g_flag_check = false;
    bool do_flush = g_flag_flush; g_flag_flush = false;
    TickType_t now = xTaskGetTickCount();

    if (do_check) {
      if (gnuboy_sram_dirty()) {
        if (g_first_dirty == 0) g_first_dirty = now;
        if (now >= g_next_allow) {
          do_flush = true;
        }
      }
    }

    if (do_flush && now >= g_next_allow) {
      bool ok = save_now();
      if (ok) {
        // Wait 15s ONLY if the save succeeded
        g_next_allow = xTaskGetTickCount() + pdMS_TO_TICKS(GAP_MS);
      } else {
        printf("[GBC][SAVE] save failed, will retry on next tick\n");
      }
    }
  }
}

// ============================================================================
// API
// ============================================================================
extern "C" void gbc_save_init(const char* romPathOrName) {
  if (!g_save_path) {
    g_save_path = (char*)malloc(PATH_MAX);
    if (!g_save_path) {
      printf("[GBC][SAVE] OOM on path alloc, autosave disabled\n");
      return;
    }
  }

  g_save_path[0] = '\0';
  make_save_path(romPathOrName);

  g_next_check  = 0;
  g_next_allow  = 0;
  g_first_dirty = 0;
  g_last_save   = 0;
  g_flag_check  = false;
  g_flag_flush  = false;

  if (!g_task) {
    xTaskCreatePinnedToCore(
      SaveTask,
      "GBC_SaveTask",
      3072,
      nullptr,
      6,
      &g_task,
      0
    );
  }

  printf("[GBC][SAVE] path=%s\n", g_save_path);
}

extern "C" void gbc_save_load(void) {
  if (!g_save_path) return;
  if (!share::gameSaveEnsureParentReady(GBC_SAVE_DIR)) {
    printf("[GBC][SAVE] skip load (storage not ready)\n");
    return;
  }

  struct stat st;
  if (stat(g_save_path, &st) != 0) {
    printf("[GBC][SAVE] no existing save file for %s\n", g_save_path);
    return;
  }

  printf("[GBC][SAVE] existing save size: %ld bytes\n", (long)st.st_size);

  int rc = gnuboy_load_sram(g_save_path);
  if (rc != 0) {
    printf("[GBC][SAVE] load failed rc=%d for %s\n", rc, g_save_path);
  } else {
    printf("[GBC][SAVE] SRAM loaded from %s\n", g_save_path);

    FILE* f = fopen(g_save_path, "rb");
    if (f) {
      uint8_t buf[16];
      size_t got = fread(buf, 1, sizeof(buf), f);
      fclose(f);
      printf("[GBC][SAVE] first bytes: ");
      for (size_t i = 0; i < got; ++i) printf("%02X ", buf[i]);
      printf("\n");
    }
  }
}

extern "C" void gbc_save_tick(void) {
  if (!g_save_path) return;
  TickType_t now = xTaskGetTickCount();
  if (now < g_next_check) return;

  g_next_check = now + pdMS_TO_TICKS(CHECK_MS);
  g_flag_check = true;

  if (g_task) xTaskNotifyGive(g_task);
}

extern "C" void gbc_save_request_flush(void) {
  g_flag_flush = true;
  if (g_task) xTaskNotifyGive(g_task);
}

extern "C" void gbc_save_force_flush(void) {
  save_now();
}

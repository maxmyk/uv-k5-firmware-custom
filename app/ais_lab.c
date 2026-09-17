#ifdef ENABLE_AIS_LAB

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app/ais_lab.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "radio.h"
#include "ui/helper.h"
#include "ui/ui.h"

// Lab builds disable DTMF calling and reuse the stock DTMF contact block.
// The complete contact block is 0x1C00..0x1DFF. This record uses only the
// first 112 bytes. Calibration starts at 0x1E00 and is never touched here.
#define AISLAB_EEPROM_BASE 0x1C00u
#define AISLAB_MAGIC 0x4C534941u // "AISL" little-endian
#define AISLAB_VERSION 2u
#define AISLAB_SLOT_COUNT 12u
#define AISLAB_ENTRY_ENABLED 0x01u
#define AISLAB_ENTRY_SEEDED 0x02u

// Do not expose register 0x00 (chip reset) or 0x01 to the field editor.
#define AISLAB_MIN_REG 0x02u
#define AISLAB_MAX_REG 0x7Fu

typedef struct __attribute__((packed)) {
  uint8_t reg;
  uint8_t flags;
  uint16_t value;
  uint16_t mask;
  uint16_t reserved;
} AISLAB_Entry_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t version;
  uint8_t globalEnabled;
  uint8_t selectedSlot;
  uint8_t stepIndex;
  AISLAB_Entry_t slot[AISLAB_SLOT_COUNT];
  uint16_t checksum;
  uint8_t padding[6];
} AISLAB_Config_t;

static_assert(sizeof(AISLAB_Entry_t) == 8, "AIS LAB entry size");
static_assert(sizeof(AISLAB_Config_t) == 112, "AIS LAB EEPROM record size");

typedef enum {
  AISLAB_FIELD_SLOT = 0,
  AISLAB_FIELD_REG,
  AISLAB_FIELD_VALUE,
  AISLAB_FIELD_MASK,
  AISLAB_FIELD_COUNT
} AISLAB_Field_t;

static AISLAB_Config_t gAISLab;
static bool gAISLabActive;
static bool gAISLabDirty;
static AISLAB_Field_t gAISLabField;
static uint32_t gAISLabInput;
static uint8_t gAISLabInputDigits;
static uint8_t gAISLabMessageTicks;
static char gAISLabMessage[10];

static const uint8_t gAISLabDefaultRegs[AISLAB_SLOT_COUNT] = {
    0x43, // RX/channel/filter bandwidths: primary AIS target
    0x2B, // FM sub-audio filtering and emphasis
    0x7E, // RX/TX DC filter settings
    0x47, // AF routing and filtering
    0x48, // AF RX gain and DAC gain
    0x3D, // modulation/DSP-related control
    0x73, // AFC-related control
    0x30, // DSP/MIC ADC-related control
    0x40, // deviation-related; mostly TX
    0x7D, // MIC sensitivity; mostly TX
    0x4B, // ALC-related
    0x19  // MIC AGC-related
};

static const uint16_t gAISLabSteps[] = {1u, 16u, 256u, 4096u};
static const char *const gAISLabStepNames[] = {"1", "16", "256", "4K"};

static uint16_t AISLAB_Checksum(const AISLAB_Config_t *cfg) {
  const uint8_t *p = (const uint8_t *)cfg;
  const size_t n = offsetof(AISLAB_Config_t, checksum);
  uint16_t sum = 0x5A5Au;

  for (size_t i = 0; i < n; i++)
    sum = (uint16_t)(((sum << 1) | (sum >> 15)) ^ p[i]);

  return sum;
}

static bool AISLAB_RegAllowed(uint8_t reg) {
  return reg >= AISLAB_MIN_REG && reg <= AISLAB_MAX_REG;
}

static bool AISLAB_IsDigRx(void) {
#ifdef ENABLE_DIGITAL_MODULATION
  return gRxVfo != NULL && gRxVfo->Modulation == MODULATION_DIGITAL;
#else
  return false;
#endif
}

static void AISLAB_SetMessage(const char *s) {
  memset(gAISLabMessage, 0, sizeof(gAISLabMessage));
  strncpy(gAISLabMessage, s, sizeof(gAISLabMessage) - 1);
  gAISLabMessageTicks = 5;
}

static void AISLAB_DefaultConfig(void) {
  memset(&gAISLab, 0, sizeof(gAISLab));
  gAISLab.magic = AISLAB_MAGIC;
  gAISLab.version = AISLAB_VERSION;
  gAISLab.globalEnabled = 0;
  gAISLab.selectedSlot = 0;
  gAISLab.stepIndex = 0;

  for (uint8_t i = 0; i < AISLAB_SLOT_COUNT; i++) {
    gAISLab.slot[i].reg = gAISLabDefaultRegs[i];
    gAISLab.slot[i].flags = 0;
    gAISLab.slot[i].value = 0;
    gAISLab.slot[i].mask = 0xFFFFu;
    gAISLab.slot[i].reserved = 0;
  }

  gAISLab.checksum = AISLAB_Checksum(&gAISLab);
  gAISLabDirty = false;
}

static void AISLAB_ReadEeprom(void) {
  uint8_t *p = (uint8_t *)&gAISLab;

  for (uint16_t offset = 0; offset < sizeof(gAISLab); offset += 8)
    EEPROM_ReadBuffer((uint16_t)(AISLAB_EEPROM_BASE + offset), p + offset, 8);

  if (gAISLab.magic != AISLAB_MAGIC || gAISLab.version != AISLAB_VERSION ||
      gAISLab.selectedSlot >= AISLAB_SLOT_COUNT ||
      gAISLab.stepIndex >= ARRAY_SIZE(gAISLabSteps) ||
      gAISLab.checksum != AISLAB_Checksum(&gAISLab)) {
    AISLAB_DefaultConfig();
  }
}

static void AISLAB_WriteEeprom(void) {
  uint8_t *p = (uint8_t *)&gAISLab;

  gAISLab.magic = AISLAB_MAGIC;
  gAISLab.version = AISLAB_VERSION;
  gAISLab.checksum = AISLAB_Checksum(&gAISLab);

  for (uint16_t offset = 0; offset < sizeof(gAISLab); offset += 8)
    EEPROM_WriteBuffer((uint16_t)(AISLAB_EEPROM_BASE + offset), p + offset);

  gAISLabDirty = false;
  AISLAB_SetMessage("SAVED");
}

static AISLAB_Entry_t *AISLAB_Current(void) {
  if (gAISLab.selectedSlot >= AISLAB_SLOT_COUNT)
    gAISLab.selectedSlot = 0;

  return &gAISLab.slot[gAISLab.selectedSlot];
}

static void AISLAB_SeedCurrent(void) {
  AISLAB_Entry_t *e = AISLAB_Current();

  if (!AISLAB_RegAllowed(e->reg))
    e->reg = 0x43;

  if ((e->flags & AISLAB_ENTRY_SEEDED) == 0) {
    e->value = BK4819_ReadRegister(e->reg);
    if (e->mask == 0)
      e->mask = 0xFFFFu;
    e->flags |= AISLAB_ENTRY_SEEDED;
    gAISLabDirty = true;
  }
}

static void AISLAB_ApplyEntry(const AISLAB_Entry_t *e) {
  if ((e->flags & AISLAB_ENTRY_ENABLED) == 0 || !AISLAB_RegAllowed(e->reg))
    return;

  const uint16_t oldValue = BK4819_ReadRegister(e->reg);
  const uint16_t newValue =
      (uint16_t)((oldValue & (uint16_t)~e->mask) | (e->value & e->mask));
  BK4819_WriteRegister(e->reg, newValue);
}

void AISLAB_ApplyOverrides(void) {
  if (!gAISLab.globalEnabled || !AISLAB_IsDigRx())
    return;

  for (uint8_t i = 0; i < AISLAB_SLOT_COUNT; i++)
    AISLAB_ApplyEntry(&gAISLab.slot[i]);
}

static void AISLAB_RebuildReceiver(void) {
  // Re-run the stock/Mobilinkd RX setup, then the hook at the end of
  // RADIO_SetupRegisters() reapplies enabled lab overrides last.
  RADIO_SetupRegisters(true);
}

static uint8_t AISLAB_Digit(KEY_Code_t key) {
  if (key <= KEY_9)
    return (uint8_t)(key - KEY_0);

  return 0xFF;
}

static uint32_t AISLAB_FieldMax(void) {
  switch (gAISLabField) {
  case AISLAB_FIELD_SLOT:
    return AISLAB_SLOT_COUNT;
  case AISLAB_FIELD_REG:
    return AISLAB_MAX_REG;
  case AISLAB_FIELD_VALUE:
    return 65535u;
  case AISLAB_FIELD_MASK:
    return 65535u;
  default:
    return 0;
  }
}

static uint32_t AISLAB_FieldValue(void) {
  const AISLAB_Entry_t *e = AISLAB_Current();

  switch (gAISLabField) {
  case AISLAB_FIELD_SLOT:
    return (uint32_t)gAISLab.selectedSlot + 1u;
  case AISLAB_FIELD_REG:
    return e->reg;
  case AISLAB_FIELD_VALUE:
    return e->value;
  case AISLAB_FIELD_MASK:
    return e->mask;
  default:
    return 0;
  }
}

static void AISLAB_SetFieldValue(uint32_t value, bool rebuild) {
  AISLAB_Entry_t *e = AISLAB_Current();

  switch (gAISLabField) {
  case AISLAB_FIELD_SLOT:
    if (value < 1u)
      value = 1u;
    if (value > AISLAB_SLOT_COUNT)
      value = AISLAB_SLOT_COUNT;
    gAISLab.selectedSlot = (uint8_t)(value - 1u);
    AISLAB_SeedCurrent();
    break;

  case AISLAB_FIELD_REG:
    if (value < AISLAB_MIN_REG)
      value = AISLAB_MIN_REG;
    if (value > AISLAB_MAX_REG)
      value = AISLAB_MAX_REG;
    e->reg = (uint8_t)value;
    e->value = BK4819_ReadRegister(e->reg);
    e->mask = 0xFFFFu;
    e->flags |= AISLAB_ENTRY_SEEDED;
    break;

  case AISLAB_FIELD_VALUE:
    e->value = (uint16_t)value;
    e->flags |= AISLAB_ENTRY_SEEDED;
    break;

  case AISLAB_FIELD_MASK:
    e->mask = (uint16_t)value;
    e->flags |= AISLAB_ENTRY_SEEDED;
    break;

  default:
    return;
  }

  gAISLabDirty = true;

  if (rebuild && gAISLab.globalEnabled && AISLAB_IsDigRx())
    AISLAB_RebuildReceiver();
}

static void AISLAB_CommitInput(void) {
  if (gAISLabInputDigits == 0)
    return;

  uint32_t value = gAISLabInput;
  const uint32_t max = AISLAB_FieldMax();
  if (value > max)
    value = max;

  AISLAB_SetFieldValue(value, true);
  gAISLabInput = 0;
  gAISLabInputDigits = 0;
}

static void AISLAB_Nudge(int direction) {
  uint32_t value = AISLAB_FieldValue();
  const uint32_t max = AISLAB_FieldMax();
  const uint32_t min = (gAISLabField == AISLAB_FIELD_SLOT)  ? 1u
                       : (gAISLabField == AISLAB_FIELD_REG) ? AISLAB_MIN_REG
                                                            : 0u;
  const uint32_t step =
      (gAISLabField == AISLAB_FIELD_VALUE || gAISLabField == AISLAB_FIELD_MASK)
          ? gAISLabSteps[gAISLab.stepIndex]
          : 1u;

  gAISLabInput = 0;
  gAISLabInputDigits = 0;

  if (direction > 0) {
    if (value < max)
      value = (max - value < step) ? max : value + step;
  } else if (value > min) {
    value = (value - min < step) ? min : value - step;
  }

  AISLAB_SetFieldValue(value, true);
}

void AISLAB_Init(void) {
  gAISLabActive = false;
  gAISLabField = AISLAB_FIELD_SLOT;
  gAISLabInput = 0;
  gAISLabInputDigits = 0;
  gAISLabMessageTicks = 0;
  gAISLabMessage[0] = '\0';
  AISLAB_ReadEeprom();
}

void AISLAB_Enter(void) {
  gAISLabActive = true;
  gAISLabField = AISLAB_FIELD_SLOT;
  gAISLabInput = 0;
  gAISLabInputDigits = 0;
  AISLAB_SeedCurrent();
  AISLAB_SetMessage(AISLAB_IsDigRx() ? "READY" : "SELECT DIG");
  gUpdateDisplay = true;
}

void AISLAB_Exit(void) {
  AISLAB_CommitInput();
  gAISLabActive = false;
  gUpdateDisplay = true;
}

bool AISLAB_IsActive(void) { return gAISLabActive; }

bool AISLAB_IsRxGuardEnabled(void) {
  return gAISLab.globalEnabled && AISLAB_IsDigRx();
}

void AISLAB_ProcessKey(KEY_Code_t key, bool keyPressed, bool keyHeld) {
  // PTT is always consumed by the lab editor itself.
  if (key == KEY_PTT) {
    if (!keyHeld && keyPressed)
      gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
    return;
  }

  // Act on key release only. It prevents repeats while tuning register values.
  if (keyPressed || keyHeld)
    return;

  const uint8_t digit = AISLAB_Digit(key);
  if (digit != 0xFF) {
    const uint32_t max = AISLAB_FieldMax();
    const uint32_t candidate = (gAISLabInput * 10u) + digit;

    if (candidate <= max && gAISLabInputDigits < 5) {
      gAISLabInput = candidate;
      gAISLabInputDigits++;
    } else {
      gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
    }

    gUpdateDisplay = true;
    return;
  }

  switch (key) {
  case KEY_MENU:
    AISLAB_CommitInput();
    gAISLabField = (AISLAB_Field_t)((gAISLabField + 1u) % AISLAB_FIELD_COUNT);
    gUpdateDisplay = true;
    break;

  case KEY_UP:
    AISLAB_Nudge(+1);
    gUpdateDisplay = true;
    break;

  case KEY_DOWN:
    AISLAB_Nudge(-1);
    gUpdateDisplay = true;
    break;

  case KEY_STAR: {
    AISLAB_CommitInput();
    AISLAB_Entry_t *e = AISLAB_Current();
    AISLAB_SeedCurrent();
    e->flags ^= AISLAB_ENTRY_ENABLED;

    if (e->flags & AISLAB_ENTRY_ENABLED)
      gAISLab.globalEnabled = 1;

    gAISLabDirty = true;
    if (AISLAB_IsDigRx())
      AISLAB_RebuildReceiver();
    gUpdateDisplay = true;
    break;
  }

  case KEY_F:
    AISLAB_CommitInput();
    AISLAB_WriteEeprom();
    gUpdateDisplay = true;
    break;

  case KEY_SIDE1:
    AISLAB_CommitInput();
    gAISLab.globalEnabled ^= 1u;
    gAISLabDirty = true;
    if (AISLAB_IsDigRx())
      AISLAB_RebuildReceiver();
    AISLAB_SetMessage(gAISLab.globalEnabled ? "LAB ON" : "LAB OFF");
    gUpdateDisplay = true;
    break;

  case KEY_SIDE2:
    AISLAB_CommitInput();
    gAISLab.stepIndex =
        (uint8_t)((gAISLab.stepIndex + 1u) % ARRAY_SIZE(gAISLabSteps));
    gAISLabDirty = true;
    sprintf(gAISLabMessage, "STEP %s", gAISLabStepNames[gAISLab.stepIndex]);
    gAISLabMessageTicks = 5;
    gUpdateDisplay = true;
    break;

  case KEY_EXIT:
    AISLAB_Exit();
    break;

  default:
    break;
  }
}

static char AISLAB_FieldMarker(AISLAB_Field_t field) {
  return (gAISLabField == field) ? '>' : ' ';
}

void AISLAB_Display(void) {
  char s[24];
  AISLAB_Entry_t *e = AISLAB_Current();
  AISLAB_SeedCurrent();

  UI_DisplayClear();

  if (gAISLabMessageTicks > 0) {
    sprintf(s, "AIS LAB %-9s", gAISLabMessage);
    gAISLabMessageTicks--;
  } else {
    sprintf(s, "AIS %s %s S%s%s", gAISLab.globalEnabled ? "ON" : "OFF",
            AISLAB_IsDigRx() ? "DIG" : "!DIG",
            gAISLabStepNames[gAISLab.stepIndex], gAISLabDirty ? "*" : "");
  }
  UI_PrintStringSmallNormal(s, 0, LCD_WIDTH, 0);

  sprintf(s, "%cSLOT %02u %s", AISLAB_FieldMarker(AISLAB_FIELD_SLOT),
          (unsigned)(gAISLab.selectedSlot + 1u),
          (e->flags & AISLAB_ENTRY_ENABLED) ? "EN" : "--");
  UI_PrintStringSmallNormal(s, 0, LCD_WIDTH, 1);

  sprintf(s, "%cREG %3u 0x%02X", AISLAB_FieldMarker(AISLAB_FIELD_REG),
          (unsigned)e->reg, (unsigned)e->reg);
  UI_PrintStringSmallNormal(s, 0, LCD_WIDTH, 2);

  sprintf(s, "%cVAL %5u %04X", AISLAB_FieldMarker(AISLAB_FIELD_VALUE),
          (unsigned)e->value, (unsigned)e->value);
  UI_PrintStringSmallNormal(s, 0, LCD_WIDTH, 3);

  sprintf(s, "%cMSK %5u %04X", AISLAB_FieldMarker(AISLAB_FIELD_MASK),
          (unsigned)e->mask, (unsigned)e->mask);
  UI_PrintStringSmallNormal(s, 0, LCD_WIDTH, 4);

  if (gAISLabInputDigits > 0) {
    sprintf(s, "INPUT %lu", (unsigned long)gAISLabInput);
  } else {
    const uint16_t hw =
        AISLAB_RegAllowed(e->reg) ? BK4819_ReadRegister(e->reg) : 0;
    sprintf(s, "HW %5u %04X", (unsigned)hw, (unsigned)hw);
  }
  UI_PrintStringSmallNormal(s, 0, LCD_WIDTH, 5);

  UI_PrintStringSmallNormal("M fld *=EN F=SAVE", 0, LCD_WIDTH, 6);

  ST7565_BlitFullScreen();
}

#endif

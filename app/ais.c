/* AIS receiver application for the UV-K5/K6.
 *
 * Uses the firmware's field-test-proven DIG/WIDE receive path and exposes only
 * the two AIS channels. The user's active VFO is snapshotted in RAM and
 * restored on exit, so AIS operation never needs to be saved into a channel.
 */

#ifdef ENABLE_AIS

#include <string.h>

#include "app/ais.h"
#include "app/app.h"
#include "app/chFrScanner.h"
#include "app/scanner.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "frequencies.h"
#include "functions.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/ui.h"

static const uint32_t gAisFrequencies[AIS_CHANNEL_COUNT] = {
    16197500u, // AIS 1 / marine 87B / 161.975 MHz
    16202500u  // AIS 2 / marine 88B / 162.025 MHz
};

static const char *const gAisMarineChannelNames[AIS_CHANNEL_COUNT] = {"87B",
                                                                      "88B"};

static const uint16_t gAisAutoSwitchIntervalsSeconds[AIS_AUTO_INTERVAL_COUNT] =
    {0u, 15u, 30u, 60u, 90u, 120u};

bool gAisMode;
uint8_t gAisChannel = 0;         // Start on AIS 1 (87B)
uint8_t gAisAutoSwitchIndex = 0; // OFF by default

static uint16_t gAisAutoSwitchCountdown_500ms;

static VFO_Info_t gAisSavedVfo;
static uint8_t gAisSavedTxVfo;
static uint8_t gAisSavedDualWatch;
static uint8_t gAisSavedCrossBand;
static bool gAisSavedKeyLock;

uint32_t AIS_GetFrequency(void) {
  return gAisFrequencies[gAisChannel % AIS_CHANNEL_COUNT];
}

const char *AIS_GetMarineChannelName(void) {
  return gAisMarineChannelNames[gAisChannel % AIS_CHANNEL_COUNT];
}

uint16_t AIS_GetAutoSwitchIntervalSeconds(void) {
  return gAisAutoSwitchIntervalsSeconds[gAisAutoSwitchIndex %
                                        AIS_AUTO_INTERVAL_COUNT];
}

uint16_t AIS_GetAutoSwitchRemainingSeconds(void) {
  if (gAisAutoSwitchCountdown_500ms == 0)
    return 0;

  return (gAisAutoSwitchCountdown_500ms + 1u) / 2u;
}

static void AIS_ResetAutoSwitchTimer(void) {
  const uint16_t seconds = AIS_GetAutoSwitchIntervalSeconds();
  gAisAutoSwitchCountdown_500ms = seconds * 2u;
  gUpdateDisplay = true;
}

static void AIS_SetFrequency(void) {
  VFO_Info_t *vfo = &gEeprom.VfoInfo[gAisSavedTxVfo];
  const uint32_t frequency = AIS_GetFrequency();

  vfo->freq_config_RX.Frequency = frequency;
  vfo->freq_config_TX.Frequency = frequency;
  vfo->Band = FREQUENCY_GetBand(frequency);

  BK4819_SetFrequency(frequency);
  BK4819_PickRXFilterPathBasedOnFrequency(frequency);
  BK4819_RX_TurnOn();

  gUpdateDisplay = true;
}

static void AIS_SwitchChannel(void) {
  gAisChannel ^= 1u;
  AIS_SetFrequency();
  AIS_ResetAutoSwitchTimer();
}

void AIS_Start(void) {
  if (gAisMode || gCurrentFunction == FUNCTION_TRANSMIT)
    return;

  if (gScanStateDir != SCAN_OFF || SCANNER_IsScanning()) {
    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
    return;
  }

  // The temporary AIS state can't leak into EEPROM.
  APP_FlushPendingSavesForAIS();

  gAisSavedTxVfo = gEeprom.TX_VFO;
  gAisSavedVfo = gEeprom.VfoInfo[gAisSavedTxVfo];
  gAisSavedDualWatch = gEeprom.DUAL_WATCH;
  gAisSavedCrossBand = gEeprom.CROSS_BAND_RX_TX;
  gAisSavedKeyLock = gEeprom.KEY_LOCK;

  gAisMode = true;

  // one receiver only
  gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
  gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
  gEeprom.KEY_LOCK = false;
  RADIO_SelectVfos();

  VFO_Info_t *vfo = &gEeprom.VfoInfo[gAisSavedTxVfo];

  vfo->FrequencyReverse = false;
  vfo->pRX = &vfo->freq_config_RX;
  vfo->pTX = &vfo->freq_config_TX;
  vfo->TX_OFFSET_FREQUENCY = 0;
  vfo->TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
  vfo->CHANNEL_BANDWIDTH = BANDWIDTH_WIDE;
  vfo->Modulation = MODULATION_DIGITAL;
  vfo->SCRAMBLING_TYPE = 0;
  vfo->Compander = 0;
  vfo->freq_config_RX.CodeType = CODE_TYPE_OFF;
  vfo->freq_config_TX.CodeType = CODE_TYPE_OFF;
#ifdef ENABLE_DTMF_CALLING
  vfo->DTMF_DECODING_ENABLE = 0;
#endif

  AIS_SetFrequency();
  AIS_ResetAutoSwitchTimer();
  RADIO_SetupRegisters(true);

  APP_StartListening(FUNCTION_MONITOR);

  GUI_SelectNextDisplay(DISPLAY_AIS);
}

void AIS_Stop(void) {
  if (!gAisMode)
    return;

  AUDIO_AudioPathOff();
  gEnableSpeaker = false;

  gEeprom.VfoInfo[gAisSavedTxVfo] = gAisSavedVfo;
  gEeprom.TX_VFO = gAisSavedTxVfo;
  gEeprom.DUAL_WATCH = gAisSavedDualWatch;
  gEeprom.CROSS_BAND_RX_TX = gAisSavedCrossBand;
  gEeprom.KEY_LOCK = gAisSavedKeyLock;

  RADIO_SelectVfos();
  RADIO_SetupRegisters(true);

  gAisAutoSwitchCountdown_500ms = 0;
  gAisMode = false;
  GUI_SelectNextDisplay(DISPLAY_MAIN);
}

void AIS_TimeSlice500ms(void) {
  if (!gAisMode)
    return;

  const uint16_t interval = AIS_GetAutoSwitchIntervalSeconds();
  if (interval == 0) {
    gAisAutoSwitchCountdown_500ms = 0;
    return;
  }

  if (gAisAutoSwitchCountdown_500ms == 0) {
    AIS_ResetAutoSwitchTimer();
    return;
  }

  gAisAutoSwitchCountdown_500ms--;

  if (gAisAutoSwitchCountdown_500ms == 0) {
    AIS_SwitchChannel();
    return;
  }

  // The displayed countdown changes only once per second.
  if ((gAisAutoSwitchCountdown_500ms & 1u) == 0)
    gUpdateDisplay = true;
}

void AIS_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) {
  // Act once on the initial press
  if (!bKeyPressed || bKeyHeld)
    return;

  switch (Key) {
  case KEY_UP:
  case KEY_DOWN:
    AIS_SwitchChannel();
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    break;

  case KEY_MENU:
    gAisAutoSwitchIndex++;
    if (gAisAutoSwitchIndex >= AIS_AUTO_INTERVAL_COUNT)
      gAisAutoSwitchIndex = 0;
    AIS_ResetAutoSwitchTimer();
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    break;

  case KEY_EXIT:
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    AIS_Stop();
    break;

  case KEY_PTT:
    // AIS mode is deliberately receive-only.
    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
    break;

  default:
    // Side-key actions, F keys, digits, etc. are intentionally
    // suppressed while the temporary AIS receive state is active.
    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
    break;
  }
}

#endif

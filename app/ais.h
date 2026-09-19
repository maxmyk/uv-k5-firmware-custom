/* AIS receiver application for the UV-K5/K6. */
#ifndef APP_AIS_H
#define APP_AIS_H

#ifdef ENABLE_AIS

#include <stdbool.h>
#include <stdint.h>
#include "driver/keyboard.h"

#define AIS_CHANNEL_COUNT 2u
#define AIS_AUTO_INTERVAL_COUNT 6u

extern bool gAisMode;
extern uint8_t gAisChannel;
extern uint8_t gAisAutoSwitchIndex;

void AIS_Start(void);
void AIS_Stop(void);
void AIS_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);
void AIS_TimeSlice500ms(void);
uint32_t AIS_GetFrequency(void);
const char *AIS_GetMarineChannelName(void);
uint16_t AIS_GetAutoSwitchIntervalSeconds(void);
uint16_t AIS_GetAutoSwitchRemainingSeconds(void);

#endif

#endif

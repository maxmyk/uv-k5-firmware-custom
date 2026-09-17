#ifndef APP_AIS_LAB_H
#define APP_AIS_LAB_H

#include <stdbool.h>

#include "driver/keyboard.h"

#ifdef ENABLE_AIS_LAB

void AISLAB_Init(void);
void AISLAB_Enter(void);
void AISLAB_Exit(void);
bool AISLAB_IsActive(void);
bool AISLAB_IsRxGuardEnabled(void);

void AISLAB_ProcessKey(KEY_Code_t key, bool keyPressed, bool keyHeld);
void AISLAB_Display(void);

// Called after the normal RX register setup. Overrides are applied only when
// the RX VFO is using Mobilinkd's DIG modulation mode.
void AISLAB_ApplyOverrides(void);

#endif

#endif

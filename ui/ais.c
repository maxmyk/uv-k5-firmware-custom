#ifdef ENABLE_AIS

#include <string.h>

#include "app/ais.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "ui/ais.h"
#include "ui/helper.h"

void UI_DisplayAIS(void) {
  char channelString[20];
  char frequencyString[12];
  char timerString[16];
  const uint32_t frequency = AIS_GetFrequency();
  const uint16_t autoSeconds = AIS_GetAutoSwitchIntervalSeconds();

  UI_DisplayClear();

  UI_PrintStringSmallBold("AIS", 0, 128, 0);

  sprintf(channelString, "AIS %u / MARINE %s", (unsigned)gAisChannel + 1u,
          AIS_GetMarineChannelName());
  UI_PrintStringSmallBold(channelString, 0, 128, 1);

  sprintf(frequencyString, "%3u.%05u", (unsigned)(frequency / 100000u),
          (unsigned)(frequency % 100000u));
  UI_PrintString(frequencyString, 0, 127, 2, 10);

  if (autoSeconds == 0) {
    UI_PrintStringSmallNormal("AUTO: OFF", 0, 128, 4);
    UI_PrintStringSmallNormal("NEXT: --", 0, 128, 5);
  } else {
    sprintf(timerString, "AUTO: %us", (unsigned)autoSeconds);
    UI_PrintStringSmallNormal(timerString, 0, 128, 4);

    sprintf(timerString, "NEXT: %us",
            (unsigned)AIS_GetAutoSwitchRemainingSeconds());
    UI_PrintStringSmallNormal(timerString, 0, 128, 5);
  }

#ifdef ENABLE_AIS_IDENTITY
  UI_PrintStringSmallNormal("@" AIS_AUTHOR_STRING " " VERSION_STRING, 0, 128,
                            6);
#else
  UI_PrintStringSmallNormal("AIS RX", 0, 128, 6);
#endif

  ST7565_BlitFullScreen();
}

#endif

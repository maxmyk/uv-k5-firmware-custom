#ifdef ENABLE_AIS

#include <string.h>

#include "app/ais.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "ui/ais.h"
#include "ui/helper.h"

void UI_DisplayAIS(void)
{
    char string[24];
    const uint32_t frequency = AIS_GetFrequency();

    UI_DisplayClear();

    UI_PrintString("AIS", 0, 127, 0, 10);

    sprintf(string, "AIS %u / MARINE %s",
            (unsigned)gAisChannel + 1u, AIS_GetMarineChannelName());
    UI_PrintStringSmallBold(string, 0, 128, 2);

    sprintf(string, "%3u.%05u",
            (unsigned)(frequency / 100000u),
            (unsigned)(frequency % 100000u));
    UI_PrintString(string, 0, 127, 3, 10);

    UI_PrintStringSmallNormal("DIG/WIDE RX", 0, 128, 4);

    const uint16_t autoSeconds = AIS_GetAutoSwitchIntervalSeconds();
    if (autoSeconds == 0) {
        UI_PrintStringSmallNormal("AUTO OFF  M=CHANGE", 0, 128, 5);
    } else {
        sprintf(string, "AUTO %us  NEXT %us",
                (unsigned)autoSeconds,
                (unsigned)AIS_GetAutoSwitchRemainingSeconds());
        UI_PrintStringSmallNormal(string, 0, 128, 5);
    }

#ifdef ENABLE_AIS_IDENTITY
    UI_PrintStringSmallNormal("@" AIS_AUTHOR_STRING " " VERSION_STRING, 0, 128, 6);
#else
    UI_PrintStringSmallNormal("AIS RX", 0, 128, 6);
#endif

    ST7565_BlitFullScreen();
}

#endif

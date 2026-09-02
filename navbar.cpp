#include <M5Unified.h>
#include "p4_os.h"

static const int NAVBAR_H = 110;

static int navBackX, navBackY, navBackW, navBackH;

int navbar_height() {
    return NAVBAR_H;
}

void navbar_draw(const char* title) {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();
    int barY = H - NAVBAR_H;

    d.fillRect(0, barY, W, NAVBAR_H, COL_STATUSBAR);
    d.drawFastHLine(0, barY, W, 0x333355U);

    navBackW = (W < 500) ? (W - 48) : 340;
    navBackH = NAVBAR_H - 24;
    navBackX = 24;
    navBackY = barY + 12;

    d.fillRoundRect(navBackX, navBackY, navBackW, navBackH, 16, os.accentColor);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, os.accentColor);
    d.drawString("< Home", navBackX + navBackW / 2, navBackY + navBackH / 2);

    if (title && title[0] && W >= 500) {
        d.setTextDatum(middle_right);
        d.setFont(&fonts::Font4);
        d.setTextColor(COL_TEXT_DIM, COL_STATUSBAR);
        d.drawString(title, W - 24, barY + NAVBAR_H / 2);
    }
}

bool navbar_touch(int tx, int ty) {
    if (tx >= navBackX && tx < navBackX + navBackW &&
        ty >= navBackY && ty < navBackY + navBackH) {
        os_goto(Screen::HOME);
        return true;
    }
    return false;
}


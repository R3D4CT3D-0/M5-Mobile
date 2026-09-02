#include <M5Unified.h>
#include "p4_os.h"
#include <Preferences.h>
#include <cstdio>
#include <cstring>

static const int  MAX_NOTES    = 10;
static const int  NOTE_MAX_LEN = 512;
static const char NVS_NS[]     = "p4notes";

static const int TITLEBAR_H  = 56;
static const int ROW_H       = 72;
static const int LIST_TOP    = TITLEBAR_H + 8;
static const int PAD         = 24;

enum class NoteView { LIST, EDIT, CONFIRM_DISCARD, CONFIRM_DELETE };
static NoteView  noteView     = NoteView::LIST;
static int       noteCount    = 0;
static char      notes[MAX_NOTES][NOTE_MAX_LEN];

static int       editIdx      = -1;
static char      editBuf[NOTE_MAX_LEN];

static int       confirmDeleteIdx = -1;

struct NoteRow { int x, y, w, h; int trashX, trashY, trashW, trashH; };
static NoteRow   noteRows[MAX_NOTES];
static int       newBtnX, newBtnY, newBtnW, newBtnH;
static int       saveBtnX, saveBtnY, saveBtnW, saveBtnH;
static int       backBtnX, backBtnY, backBtnW, backBtnH;

static int       dlgOkX, dlgOkY, dlgCancelX, dlgCancelY, dlgBtnW, dlgBtnH;

static bool in_rect(int tx, int ty, int x, int y, int w, int h) {
    return tx >= x && tx < x+w && ty >= y && ty < y+h;
}
static void draw_button(int x, int y, int w, int h,
                         const char* label, uint32_t bg, uint32_t fg) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, w, h, 8, bg);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font2);
    d.setTextColor(fg, bg);
    d.drawString(label, x + w/2, y + h/2);
}

static void notepad_load() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) { noteCount = 0; return; }
    noteCount = (int)p.getUChar("count", 0);
    if (noteCount > MAX_NOTES) noteCount = MAX_NOTES;
    for (int i = 0; i < noteCount; i++) {
        char key[8]; snprintf(key, sizeof(key), "note%d", i);
        String s = p.getString(key, "");
        strncpy(notes[i], s.c_str(), NOTE_MAX_LEN - 1);
        notes[i][NOTE_MAX_LEN - 1] = '\0';
    }
    p.end();
}

static void notepad_save_all() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putUChar("count", (uint8_t)noteCount);
    for (int i = 0; i < noteCount; i++) {
        char key[8]; snprintf(key, sizeof(key), "note%d", i);
        p.putString(key, notes[i]);
    }

    for (int i = noteCount; i < MAX_NOTES; i++) {
        char key[8]; snprintf(key, sizeof(key), "note%d", i);
        p.remove(key);
    }
    p.end();
}

static void note_title(int idx, char* out, int outLen) {
    if (idx < 0 || idx >= noteCount || notes[idx][0] == '\0') {
        strncpy(out, "(empty)", outLen - 1);
        out[outLen - 1] = '\0';
        return;
    }
    const char* nl = strchr(notes[idx], '\n');
    int len = nl ? (int)(nl - notes[idx]) : (int)strlen(notes[idx]);
    if (len >= outLen) len = outLen - 1;
    strncpy(out, notes[idx], len);
    out[len] = '\0';
}

static void draw_titlebar(const char* title) {
    auto& d = M5.Display;
    int W = d.width();
    d.fillRect(0, 0, W, TITLEBAR_H, COL_STATUSBAR);
    d.drawFastHLine(0, TITLEBAR_H, W, 0x333355U);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_STATUSBAR);
    d.drawString(title, W/2, TITLEBAR_H/2);
}

static void draw_list() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillScreen(COL_SURFACE);
    draw_titlebar("Notepad");

    newBtnW = 180; newBtnH = 38;
    newBtnX = W - PAD - newBtnW;
    newBtnY = (TITLEBAR_H - newBtnH) / 2;
    draw_button(newBtnX, newBtnY, newBtnW, newBtnH, "+ New Note", os.accentColor, COL_WHITE);

    int contentBottom = H - navbar_height();

    if (noteCount == 0) {
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font4);
        d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
        d.drawString("No notes yet. Tap + New Note to start.", W/2, (LIST_TOP + contentBottom)/2);
    } else {
        for (int i = 0; i < noteCount; i++) {
            int y = LIST_TOP + i * ROW_H;
            uint32_t rowBg = (i % 2 == 0) ? COL_CARD : COL_SURFACE;

            d.fillRect(PAD, y, W - PAD*2, ROW_H - 4, rowBg);

            char title[80];
            note_title(i, title, sizeof(title));
            d.setTextDatum(middle_left);
            d.setFont(&fonts::Font4);
            d.setTextColor(COL_WHITE, rowBg);
            d.drawString(title, PAD + 16, y + ROW_H/2 - 2);

            int len = (int)strlen(notes[i]);
            char preview[32];
            snprintf(preview, sizeof(preview), "%d chars", len);
            d.setTextDatum(middle_right);
            d.setFont(&fonts::Font2);
            d.setTextColor(COL_TEXT_DIM, rowBg);

            int tW = 60, tH = ROW_H - 16;
            int tX = W - PAD - tW;
            int tY = y + 8;
            noteRows[i] = { PAD, y, W - PAD*2 - tW - 8, ROW_H - 4,
                            tX, tY, tW, tH };
            d.drawString(preview, tX - 12, y + ROW_H/2 - 2);

            draw_button(tX, tY, tW, tH, "Del", COL_DANGER, COL_WHITE);
        }
    }

    if (noteCount >= MAX_NOTES) {

        draw_button(newBtnX, newBtnY, newBtnW, newBtnH, "+ New Note", 0x444455U, COL_TEXT_DIM);
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font2);
        d.setTextColor(COL_TEXT_DIM, COL_SURFACE);
        char msg[48];
        snprintf(msg, sizeof(msg), "Max %d notes reached. Delete one to add more.", MAX_NOTES);
        d.drawString(msg, W/2, LIST_TOP + noteCount * ROW_H + 24);
    }

    navbar_draw("Notepad");
}

static void draw_edit() {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillScreen(COL_SURFACE);
    draw_titlebar(editIdx < 0 ? "New Note" : "Edit Note");

    backBtnW = 120; backBtnH = 38;
    backBtnX = PAD; backBtnY = (TITLEBAR_H - backBtnH)/2;
    draw_button(backBtnX, backBtnY, backBtnW, backBtnH, "< Back", COL_CARD, COL_WHITE);

    saveBtnW = 120; saveBtnH = 38;
    saveBtnX = W - PAD - saveBtnW; saveBtnY = backBtnY;
    draw_button(saveBtnX, saveBtnY, saveBtnW, saveBtnH, "Save", COL_OK, COL_WHITE);

    int kbH  = keyboard_height();
    int areaTop    = TITLEBAR_H + 8;
    int areaBottom = H - kbH - 8;
    int areaH      = areaBottom - areaTop;
    if (areaH < 20) areaH = 20;

    d.fillRect(PAD, areaTop, W - PAD*2, areaH, COL_CARD);
    d.drawRoundRect(PAD, areaTop, W - PAD*2, areaH, 6, 0x333355U);

    d.setFont(&fonts::Font2);
    int lineH = 22;
    int maxLines = (areaH - 12) / lineH;

    // split the buffer on '\n' and only render the tail that fits the visible area
    const char* lineStarts[128];
    int lineCount = 0;
    const char* p = editBuf;
    lineStarts[lineCount++] = p;
    while (*p && lineCount < 128) {
        if (*p == '\n') lineStarts[lineCount++] = p + 1;
        p++;
    }
    int firstLine = (lineCount > maxLines) ? lineCount - maxLines : 0;

    d.setTextDatum(top_left);
    d.setTextColor(COL_WHITE, COL_CARD);
    for (int i = firstLine; i < lineCount; i++) {
        const char* ls = lineStarts[i];
        const char* le = (i + 1 < lineCount) ? lineStarts[i+1] - 1 : ls + strlen(ls);
        int len = (int)(le - ls);
        if (len < 0) len = 0;

        char lineBuf[160];
        int copy = len < 158 ? len : 158;
        strncpy(lineBuf, ls, copy);
        lineBuf[copy] = '\0';
        int drawY = areaTop + 6 + (i - firstLine) * lineH;
        d.drawString(lineBuf, PAD + 8, drawY);
    }

    {
        int curLine = (lineCount - 1) - firstLine;
        if (curLine >= 0 && curLine < maxLines) {
            int curY = areaTop + 6 + curLine * lineH;

            const char* ls = lineStarts[lineCount - 1];
            int len = (int)strlen(ls);
            char tmp[160]; int c = len < 158 ? len : 158;
            strncpy(tmp, ls, c); tmp[c] = '\0';
            int tw = d.textWidth(tmp);
            d.fillRect(PAD + 8 + tw, curY, 2, lineH - 2, os.accentColor);
        }
    }

    keyboard_draw();
}

static void draw_confirm(const char* message) {
    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    d.fillRect(0, 0, W, H, 0x0000009FU);

    d.fillRect(0, 0, W, H, COL_SURFACE);
    draw_titlebar("Notepad");

    int cw = 680, ch = 220;
    int cx = (W - cw)/2, cy = (H - ch)/2;
    d.fillRoundRect(cx, cy, cw, ch, 16, COL_CARD);
    d.drawRoundRect(cx, cy, cw, ch, 16, COL_DANGER);

    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_CARD);
    d.drawString(message, W/2, cy + 70);

    dlgBtnW = 200; dlgBtnH = 50;
    dlgCancelX = cx + 60;          dlgCancelY = cy + ch - 70;
    dlgOkX     = cx + cw - 60 - dlgBtnW; dlgOkY = dlgCancelY;

    draw_button(dlgCancelX, dlgCancelY, dlgBtnW, dlgBtnH, "Cancel", 0x444455U, COL_WHITE);
    draw_button(dlgOkX,     dlgOkY,     dlgBtnW, dlgBtnH,
                (noteView == NoteView::CONFIRM_DELETE) ? "Delete" : "Discard",
                COL_DANGER, COL_WHITE);
}

void notepad_app_draw() {
    switch (noteView) {
        case NoteView::LIST:            draw_list();  break;
        case NoteView::EDIT:            draw_edit();  break;
        case NoteView::CONFIRM_DISCARD: draw_confirm("Discard changes?"); break;
        case NoteView::CONFIRM_DELETE:  draw_confirm("Delete this note?"); break;
    }
}

void notepad_app_update() {
    auto t = M5.Touch.getDetail();
    if (!t.wasClicked()) return;
    int tx = t.x, ty = t.y;

    if (keyboard_is_open()) {
        int kbTop = M5.Display.height() - keyboard_height();
        if (ty >= kbTop) {
            KeyboardResult kr = keyboard_touch(tx, ty);
            if (kr.submitted) {

                if (editIdx < 0) {

                    if (noteCount < MAX_NOTES && editBuf[0] != '\0') {
                        strncpy(notes[noteCount], editBuf, NOTE_MAX_LEN - 1);
                        notes[noteCount][NOTE_MAX_LEN - 1] = '\0';
                        noteCount++;
                        notepad_save_all();
                    }
                } else {
                    strncpy(notes[editIdx], editBuf, NOTE_MAX_LEN - 1);
                    notes[editIdx][NOTE_MAX_LEN - 1] = '\0';
                    notepad_save_all();
                }
                keyboard_close();
                noteView = NoteView::LIST;
                notepad_app_draw();
            } else if (kr.cancelled) {

                const char* original = (editIdx >= 0) ? notes[editIdx] : "";
                if (strcmp(editBuf, original) != 0 && editBuf[0] != '\0') {
                    keyboard_close();
                    noteView = NoteView::CONFIRM_DISCARD;
                    notepad_app_draw();
                } else {
                    keyboard_close();
                    noteView = NoteView::LIST;
                    notepad_app_draw();
                }
            } else if (kr.changed) {
                draw_edit();
            }
            return;
        }

    }

    switch (noteView) {

        case NoteView::LIST: {
            if (navbar_touch(tx, ty)) return;

            if (noteCount < MAX_NOTES &&
                in_rect(tx, ty, newBtnX, newBtnY, newBtnW, newBtnH)) {
                editIdx = -1;
                editBuf[0] = '\0';
                keyboard_open(editBuf, NOTE_MAX_LEN, false, "Done", "Note text:");
                noteView = NoteView::EDIT;
                notepad_app_draw();
                return;
            }

            for (int i = 0; i < noteCount; i++) {

                if (in_rect(tx, ty, noteRows[i].trashX, noteRows[i].trashY,
                                    noteRows[i].trashW, noteRows[i].trashH)) {
                    confirmDeleteIdx = i;
                    noteView = NoteView::CONFIRM_DELETE;
                    notepad_app_draw();
                    return;
                }

                if (in_rect(tx, ty, noteRows[i].x, noteRows[i].y,
                                    noteRows[i].w, noteRows[i].h)) {
                    editIdx = i;
                    strncpy(editBuf, notes[i], NOTE_MAX_LEN - 1);
                    editBuf[NOTE_MAX_LEN - 1] = '\0';
                    keyboard_open(editBuf, NOTE_MAX_LEN, false, "Done", "Note text:");
                    noteView = NoteView::EDIT;
                    notepad_app_draw();
                    return;
                }
            }
            break;
        }

        case NoteView::EDIT: {

            if (in_rect(tx, ty, saveBtnX, saveBtnY, saveBtnW, saveBtnH)) {
                if (editIdx < 0) {
                    if (noteCount < MAX_NOTES && editBuf[0] != '\0') {
                        strncpy(notes[noteCount++], editBuf, NOTE_MAX_LEN - 1);
                        notepad_save_all();
                    }
                } else {
                    strncpy(notes[editIdx], editBuf, NOTE_MAX_LEN - 1);
                    notepad_save_all();
                }
                keyboard_close();
                noteView = NoteView::LIST;
                notepad_app_draw();
            } else if (in_rect(tx, ty, backBtnX, backBtnY, backBtnW, backBtnH)) {
                const char* original = (editIdx >= 0) ? notes[editIdx] : "";
                if (strcmp(editBuf, original) != 0 && editBuf[0] != '\0') {
                    keyboard_close();
                    noteView = NoteView::CONFIRM_DISCARD;
                    notepad_app_draw();
                } else {
                    keyboard_close();
                    noteView = NoteView::LIST;
                    notepad_app_draw();
                }
            }
            break;
        }

        case NoteView::CONFIRM_DISCARD: {
            if (in_rect(tx, ty, dlgOkX, dlgOkY, dlgBtnW, dlgBtnH)) {
                noteView = NoteView::LIST;
                notepad_app_draw();
            } else if (in_rect(tx, ty, dlgCancelX, dlgCancelY, dlgBtnW, dlgBtnH)) {

                keyboard_open(editBuf, NOTE_MAX_LEN, false, "Done", "Note text:");
                noteView = NoteView::EDIT;
                notepad_app_draw();
            }
            break;
        }

        case NoteView::CONFIRM_DELETE: {
            if (in_rect(tx, ty, dlgOkX, dlgOkY, dlgBtnW, dlgBtnH)) {

                if (confirmDeleteIdx >= 0 && confirmDeleteIdx < noteCount) {
                    // shift everything after the deleted note down by one slot
                    for (int i = confirmDeleteIdx; i < noteCount - 1; i++)
                        memcpy(notes[i], notes[i+1], NOTE_MAX_LEN);
                    noteCount--;
                    notepad_save_all();
                }
                confirmDeleteIdx = -1;
                noteView = NoteView::LIST;
                notepad_app_draw();
            } else if (in_rect(tx, ty, dlgCancelX, dlgCancelY, dlgBtnW, dlgBtnH)) {
                confirmDeleteIdx = -1;
                noteView = NoteView::LIST;
                notepad_app_draw();
            }
            break;
        }
    }
}

void notepad_init() {
    notepad_load();
    noteView = NoteView::LIST;
}


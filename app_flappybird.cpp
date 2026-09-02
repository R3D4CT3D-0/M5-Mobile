#include <M5Unified.h>
#include "p4_os.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

static const int TITLEBAR_H = 56;

static const float GRAVITY      = 1600.0f;
static const float FLAP_VEL     = -520.0f;
static const float MAX_FALL_VEL = 900.0f;
static const int   BIRD_R       = 20;
static const int   BIRD_X       = 220;

static const int   PIPE_W       = 110;
static const int   PIPE_GAP     = 230;
static const float PIPE_SPEED   = 260.0f;
static const int   PIPE_SPACING = 420;

static const uint32_t COL_SKY      = 0x7FD4EEU;
static const uint32_t COL_GROUND   = 0xDED895U;
static const uint32_t COL_GROUND2  = 0xC7BE6EU;
static const uint32_t COL_PIPE     = 0x4CAF50U;
static const uint32_t COL_PIPE_DK  = 0x357A38U;
static const uint32_t COL_BIRD     = 0xFFD54FU;
static const uint32_t COL_BIRD_DK  = 0xE0A800U;

enum class GameState { READY, PLAYING, DEAD };

struct Pipe {
    float x;
    int   gapY;
    bool  scored;
    int   lastDrawnX;
};

static GameState state = GameState::READY;
static float birdY, birdVel;
static float groundScrollX = 0.0f;
static float lastGroundDrawScrollX = -1.0f;
static Pipe  pipes[6];
static int   pipeCount = 0;
static int   score = 0;
static int   bestScore = 0;
static uint32_t lastFrameMs = 0;
static int   playW, playH;
static int   playTop;
static int   groundY;
static float prevBirdY;
static int   lastDrawnScore = -1;

static LGFX_Sprite s_groundSprite;
static bool        s_groundSpriteReady = false;

static int dirtyLimitY = 0;
static bool s_pipeMovedThisFrame = false;

static void draw_titlebar();
static void reset_game();
static void spawn_pipe(float x);
static void draw_scene();
static void draw_static_bg();
static void erase_sky_rect(int x, int y, int w, int h);
static void draw_bird();
static void draw_pipe(const Pipe& p);
static void draw_pipe_range(const Pipe& p, int clipX0, int clipX1);
static void render_pipe_delta(Pipe& p);
static void draw_ground();
static void draw_hud(bool force);
static void draw_ready_overlay();
static void draw_dead_overlay();
static void flap();

void flappy_app_draw() {

    rotation_set_auto(false);
    if (M5.Display.width() > M5.Display.height()) {

        M5.Display.setRotation(0);
    }

    auto& d = M5.Display;
    int W = d.width();
    int H = d.height();

    playTop = TITLEBAR_H;
    playW   = W;
    playH   = H - navbar_height() - TITLEBAR_H;
    groundY = playTop + playH - 46;

    {
        int tileRowH = H / GFX_TILE_ROWS;
        if (tileRowH <= 0) tileRowH = 1;
        dirtyLimitY = (groundY / tileRowH) * tileRowH;
        if (dirtyLimitY > groundY) dirtyLimitY = groundY;
    }

    draw_titlebar();
    navbar_draw("Flappy Bird");

    reset_game();
    draw_static_bg();
    draw_scene();
    lastFrameMs = millis();
}

static void draw_titlebar() {
    auto& d = M5.Display;
    int W = d.width();

    d.fillRect(0, 0, W, TITLEBAR_H, COL_STATUSBAR);
    d.drawFastHLine(0, TITLEBAR_H, W, 0x333355U);

    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_STATUSBAR);
    d.drawString("Flappy Bird", W / 2, TITLEBAR_H / 2);
}

static void reset_game() {
    birdY   = playTop + playH / 2.0f;
    prevBirdY = birdY;
    birdVel = 0.0f;
    score   = 0;
    lastDrawnScore = -1;
    state   = GameState::READY;
    groundScrollX = 0.0f;
    lastGroundDrawScrollX = -1.0f;

    pipeCount = 0;
    float startX = playW + 200.0f;
    for (int i = 0; i < 5; i++) {
        spawn_pipe(startX + i * PIPE_SPACING);
    }
}

static const int PIPE_MAX_GAP_JUMP = 160;

static void spawn_pipe(float x) {
    if (pipeCount >= (int)(sizeof(pipes) / sizeof(pipes[0]))) return;
    Pipe& p = pipes[pipeCount++];
    p.x = x;
    int margin = 90;
    int lo = playTop + margin + PIPE_GAP / 2;
    int hi = playTop + playH - margin - PIPE_GAP / 2;
    if (hi < lo) hi = lo;

    // keep the gap close to the previous pipe's gap so consecutive pipes are
    // actually flyable instead of the bird needing a huge vertical jump
    if (pipeCount > 1) {

        int prevGapY = pipes[0].gapY;
        float bestX  = pipes[0].x;
        for (int i = 1; i < pipeCount - 1; i++) {
            if (pipes[i].x > bestX) { bestX = pipes[i].x; prevGapY = pipes[i].gapY; }
        }
        int clo = prevGapY - PIPE_MAX_GAP_JUMP;
        int chi = prevGapY + PIPE_MAX_GAP_JUMP;
        if (clo > lo) lo = clo;
        if (chi < hi) hi = chi;
        if (hi < lo) hi = lo;
    }

    p.gapY = lo + (hi > lo ? (rand() % (hi - lo)) : 0);
    p.scored = false;
    p.lastDrawnX = (int)x;
}

static void flap() {
    if (state == GameState::READY) {
        state = GameState::PLAYING;
        birdVel = FLAP_VEL;
    } else if (state == GameState::PLAYING) {
        birdVel = FLAP_VEL;
    }
}

static void draw_static_bg() {
    auto& d = M5.Display;

    d.fillRect(0, playTop, playW, dirtyLimitY - playTop, COL_SKY);
}

static void erase_sky_rect(int x, int y, int w, int h) {
    auto& d = M5.Display;
    if (x < 0) { w += x; x = 0; }
    if (y < playTop) { h += (y - playTop); y = playTop; }
    if (x + w > playW) w = playW - x;

    if (y + h > dirtyLimitY) h = dirtyLimitY - y;
    if (w <= 0 || h <= 0) return;
    d.fillRect(x, y, w, h, COL_SKY);
}

static void draw_scene() {
    draw_static_bg();
    for (int i = 0; i < pipeCount; i++) draw_pipe(pipes[i]);
    draw_ground();
    draw_bird();
    lastDrawnScore = -1;
    draw_hud(true);

    if (state == GameState::READY) draw_ready_overlay();
    if (state == GameState::DEAD)  draw_dead_overlay();
}

static void fillRectClippedX(int rx, int ry, int rw, int rh, uint32_t col, int clipX0, int clipX1) {

    if (ry + rh > dirtyLimitY) rh = dirtyLimitY - ry;
    if (rh <= 0) return;
    int x0 = rx > clipX0 ? rx : clipX0;
    int x1 = (rx + rw) < clipX1 ? (rx + rw) : clipX1;
    if (x1 <= x0) return;
    auto& d = M5.Display;
    d.fillRect(x0, ry, x1 - x0, rh, col);
}

static void draw_pipe(const Pipe& p) {
    draw_pipe_range(p, (int)p.x - 6, (int)p.x + PIPE_W);
}

static void draw_pipe_range(const Pipe& p, int clipX0, int clipX1) {
    if (p.x + PIPE_W < 0 || p.x > playW) return;
    if (clipX1 <= clipX0) return;

    int gapTop = p.gapY - PIPE_GAP / 2;
    int gapBot = p.gapY + PIPE_GAP / 2;
    int px = (int)p.x;

    fillRectClippedX(px, playTop, PIPE_W, gapTop - playTop, COL_PIPE, clipX0, clipX1);
    fillRectClippedX(px, gapTop - 24, PIPE_W, 24, COL_PIPE_DK, clipX0, clipX1);
    fillRectClippedX(px - 6, playTop, 6, gapTop - playTop, COL_PIPE_DK, clipX0, clipX1);

    fillRectClippedX(px, gapBot, PIPE_W, groundY - gapBot, COL_PIPE, clipX0, clipX1);
    fillRectClippedX(px, gapBot, PIPE_W, 24, COL_PIPE_DK, clipX0, clipX1);
    fillRectClippedX(px - 6, gapBot, 6, groundY - gapBot, COL_PIPE_DK, clipX0, clipX1);
}

static const int PIPE_EDGE_PAD = 24;

// redraws only the sliver of sky uncovered by the pipe's movement instead of
// erasing and repainting the whole pipe every frame
static void render_pipe_delta(Pipe& p) {
    int newX = (int)p.x;
    int oldX = p.lastDrawnX;
    p.lastDrawnX = newX;
    if (newX != oldX) s_pipeMovedThisFrame = true;

    bool oldVisible = (oldX + PIPE_W >= 0 && oldX <= playW);
    bool newVisible = (newX + PIPE_W >= 0 && newX <= playW);
    if (!oldVisible && !newVisible) return;

    if (!oldVisible) { draw_pipe(p); return; }
    if (!newVisible) {
        erase_sky_rect(oldX - 6, playTop, PIPE_W + 6, groundY - playTop);
        return;
    }

    int delta = oldX - newX;
    if (delta <= 0 || delta > PIPE_W + 6) { draw_pipe(p); return; }

    erase_sky_rect(oldX + PIPE_W - delta, playTop, delta, groundY - playTop);

    int winX0 = newX - 6 - PIPE_EDGE_PAD;
    int winX1 = newX - 6 + PIPE_EDGE_PAD;
    erase_sky_rect(winX0, playTop, winX1 - winX0, groundY - playTop);
    draw_pipe_range(p, winX0, winX1);
}

static void fillSpriteRectClippedX(LGFX_Sprite& spr, int rx, int ry, int rw, int rh,
                                    uint32_t col, int clipX0, int clipX1) {
    if (rh <= 0) return;
    int x0 = rx > clipX0 ? rx : clipX0;
    int x1 = (rx + rw) < clipX1 ? (rx + rw) : clipX1;
    if (x1 <= x0) return;
    spr.fillRect(x0, ry, x1 - x0, rh, col);
}

static void draw_pipe_footer_slice(LGFX_Sprite& spr, const Pipe& p, int sliverTop, int sliverBot) {
    if (p.x + PIPE_W < 0 || p.x > playW) return;
    int gapBot = p.gapY + PIPE_GAP / 2;
    int px = (int)p.x;

    int bodyTop = gapBot > sliverTop ? gapBot : sliverTop;
    int bodyH   = sliverBot - bodyTop;
    if (bodyH > 0) {
        fillSpriteRectClippedX(spr, px,     bodyTop - dirtyLimitY, PIPE_W, bodyH, COL_PIPE,    0, playW);
        fillSpriteRectClippedX(spr, px - 6, bodyTop - dirtyLimitY, 6,      bodyH, COL_PIPE_DK, 0, playW);
    }

    if (gapBot >= sliverTop) {
        int rimTop = gapBot;
        int rimBot = gapBot + 24 < sliverBot ? gapBot + 24 : sliverBot;
        if (rimBot > rimTop)
            fillSpriteRectClippedX(spr, px, rimTop - dirtyLimitY, PIPE_W, rimBot - rimTop, COL_PIPE_DK, 0, playW);
    }
}

// the ground (and any pipe footers overlapping it) is rendered once into an
// offscreen sprite and then blitted with a scrolling offset, rather than
// redrawing every tuft of grass each frame
static void draw_ground() {
    int W       = playW;
    int groundH = playTop + playH - groundY;
    int sliverH = groundY - dirtyLimitY;
    if (sliverH < 0) sliverH = 0;
    int totalH  = sliverH + groundH;

    bool scrollMoved = (int)groundScrollX != (int)lastGroundDrawScrollX;
    bool needRedraw  = scrollMoved || s_pipeMovedThisFrame;
    s_pipeMovedThisFrame = false;
    if (!needRedraw) return;
    lastGroundDrawScrollX = groundScrollX;

    int scrollNow = (int)groundScrollX % 40;

    if (!s_groundSpriteReady || s_groundSprite.width() != W || s_groundSprite.height() != totalH) {
        if (s_groundSprite.width() > 0) s_groundSprite.deleteSprite();
        s_groundSprite.setColorDepth(16);
        s_groundSprite.setPsram(true);
        s_groundSprite.createSprite(W, totalH);
        s_groundSpriteReady = (s_groundSprite.getBuffer() != nullptr);
    }

    if (!s_groundSpriteReady) {

        auto& d = M5.Display;
        if (sliverH > 0) {
            d.fillRect(0, dirtyLimitY, W, sliverH, COL_SKY);
            for (int i = 0; i < pipeCount; i++) {
                Pipe& p = pipes[i];
                if (p.x + PIPE_W < 0 || p.x > playW) continue;
                int gapBot = p.gapY + PIPE_GAP / 2;
                int px = (int)p.x;
                int y0 = gapBot > dirtyLimitY ? gapBot : dirtyLimitY;
                int bodyH = groundY - y0;
                if (bodyH > 0) {
                    d.fillRect(px, y0, PIPE_W, bodyH, COL_PIPE);
                    d.fillRect(px - 6, y0, 6, bodyH, COL_PIPE_DK);

                    if (gapBot >= dirtyLimitY) {
                        int rimY1 = (gapBot + 24) < groundY ? (gapBot + 24) : groundY;
                        if (rimY1 > gapBot) d.fillRect(px, gapBot, PIPE_W, rimY1 - gapBot, COL_PIPE_DK);
                    }
                }
            }
        }
        d.fillRect(0, groundY, W, groundH, COL_GROUND);
        d.fillRect(0, groundY, W, 8, COL_GROUND2);
        for (int x = -scrollNow; x < W; x += 40)
            d.fillRect(x, groundY + 8, 20, groundH - 8, COL_GROUND2);
        return;
    }

    if (sliverH > 0) {
        s_groundSprite.fillRect(0, 0, W, sliverH, COL_SKY);
        for (int i = 0; i < pipeCount; i++)
            draw_pipe_footer_slice(s_groundSprite, pipes[i], dirtyLimitY, groundY);
    }

    s_groundSprite.fillRect(0, sliverH, W, groundH, COL_GROUND);
    s_groundSprite.fillRect(0, sliverH, W, 8,       COL_GROUND2);
    for (int x = -scrollNow; x < W; x += 40)
        s_groundSprite.fillRect(x, sliverH + 8, 20, groundH - 8, COL_GROUND2);

    gfx_push_direct(0, dirtyLimitY, W, totalH,
                    (const uint16_t*)s_groundSprite.getBuffer());
}

static void bird_bbox(int by, int& x, int& y, int& w, int& h) {
    x = BIRD_X - BIRD_R - 8;
    y = by - BIRD_R - 8;
    w = (BIRD_R + 14 + 2) + (BIRD_R + 8);
    h = (BIRD_R + 8) * 2;
}

static void draw_bird() {
    auto& d = M5.Display;
    int bx = BIRD_X;
    int by = (int)birdY;

    int bbX, bbY, bbW, bbH;
    bird_bbox(by, bbX, bbY, bbW, bbH);

    if (bbY + bbH > dirtyLimitY) bbH = dirtyLimitY - bbY;
    if (bbH > 0) d.markDirty(bbX, bbY, bbW, bbH);

    d.fillCircle(bx, by, BIRD_R, COL_BIRD);
    d.fillCircle(bx + 6, by - 4, 5, COL_WHITE);
    d.fillCircle(bx + 8, by - 4, 2, COL_BLACK);

    d.fillTriangle(bx + BIRD_R + 14, by,
                    bx + BIRD_R - 2, by - 6,
                    bx + BIRD_R - 2, by + 6,
                    0xFF7043U);

    int wingOffset = (birdVel < 0) ? -6 : 6;
    d.fillEllipse(bx - 6, by + wingOffset, 12, 8, COL_BIRD_DK);
}

static void draw_hud(bool force) {
    if (!force && score == lastDrawnScore) return;
    lastDrawnScore = score;

    auto& d = M5.Display;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", score);
    d.setTextDatum(top_center);
    d.setFont(&fonts::Font8);
    int cx = playW / 2, cy = playTop + 14;

    d.fillRect(cx - 70, cy - 4, 140, 60, COL_SKY);
    d.setTextColor(COL_BLACK);
    for (int dx = -2; dx <= 2; dx += 2)
        for (int dy = -2; dy <= 2; dy += 2)
            if (dx || dy) d.drawString(buf, cx + dx, cy + dy);
    d.setTextColor(COL_WHITE);
    d.drawString(buf, cx, cy);
}

static void draw_ready_overlay() {
    auto& d = M5.Display;
    int cx = playW / 2, cy = playTop + playH / 2;
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_BLACK);
    d.drawString(" Tap to flap! ", cx, cy + 60);
}

static void draw_dead_overlay() {
    auto& d = M5.Display;
    int W = playW;
    int cx = W / 2, cy = playTop + playH / 2;

    d.fillRoundRect(cx - 200, cy - 120, 400, 260, 20, COL_SURFACE);
    d.drawRoundRect(cx - 200, cy - 120, 400, 260, 20, COL_WHITE);

    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font4);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString("Game Over", cx, cy - 70);

    char buf[48];
    d.setFont(&fonts::Font4);
    snprintf(buf, sizeof(buf), "Score: %d", score);
    d.setTextColor(COL_WHITE, COL_SURFACE);
    d.drawString(buf, cx, cy - 28);
    snprintf(buf, sizeof(buf), "Best: %d", bestScore);
    d.drawString(buf, cx, cy + 4);

    int bw = 260, bh = 70;
    int bx = cx - bw / 2, by = cy + 30;
    d.fillRoundRect(bx, by, bw, bh, 16, os.accentColor);
    d.setTextColor(COL_WHITE, os.accentColor);
    d.drawString("Play Again", cx, by + bh / 2);
}

void flappy_app_update() {
    uint32_t now = millis();
    float dt = (now - lastFrameMs) / 1000.0f;
    if (dt > 0.05f) dt = 0.05f;
    lastFrameMs = now;

    auto t = M5.Touch.getDetail();

    if (t.wasClicked() && navbar_touch(t.x, t.y)) return;

    if (state == GameState::DEAD) {
        if (t.wasClicked()) {
            int W = playW;
            int cx = W / 2, cy = playTop + playH / 2;
            int bw = 260, bh = 70;
            int bx = cx - bw / 2, by = cy + 30;
            if (t.x >= bx && t.x < bx + bw && t.y >= by && t.y < by + bh) {
                reset_game();
                draw_scene();
            }
        }
        return;
    }

    if (t.wasPressed() && t.y < groundY) {

        flap();
    }

    if (state == GameState::READY) {

        int oldBy = (int)birdY;
        birdY = playTop + playH / 2.0f + 8.0f * sinf(now / 300.0f);
        int newBy = (int)birdY;
        if (oldBy != newBy) {
            int ebx, eby, ebw, ebh;
            bird_bbox(oldBy, ebx, eby, ebw, ebh);
            erase_sky_rect(ebx, eby, ebw, ebh);
            draw_bird();
        }
        return;
    }

    int oldBirdY = (int)prevBirdY;
    prevBirdY = birdY;

    birdVel += GRAVITY * dt;
    if (birdVel > MAX_FALL_VEL) birdVel = MAX_FALL_VEL;
    birdY += birdVel * dt;

    groundScrollX += PIPE_SPEED * dt;

    bool dead = false;

    if (birdY - BIRD_R <= playTop) {
        birdY = playTop + BIRD_R;
        dead = true;
    }
    if (birdY + BIRD_R >= groundY) {
        birdY = groundY - BIRD_R;
        dead = true;
    }

    int ebx, eby, ebw, ebh;
    bird_bbox(oldBirdY, ebx, eby, ebw, ebh);
    erase_sky_rect(ebx, eby, ebw, ebh);

    for (int i = 0; i < pipeCount; i++) {
        Pipe& p = pipes[i];
        int px0 = (int)p.x - 6, px1 = (int)p.x + PIPE_W;
        if (px1 > ebx && px0 < ebx + ebw) {
            int clipX0 = px0 > ebx ? px0 : ebx;
            int clipX1 = px1 < (ebx + ebw) ? px1 : (ebx + ebw);
            draw_pipe_range(p, clipX0, clipX1);
        }
    }

    for (int i = 0; i < pipeCount; i++) {
        Pipe& p = pipes[i];
        p.x -= PIPE_SPEED * dt;

        if (!p.scored && p.x + PIPE_W < BIRD_X - BIRD_R) {
            p.scored = true;
            score++;
        }

        int gapTop = p.gapY - PIPE_GAP / 2;
        int gapBot = p.gapY + PIPE_GAP / 2;
        bool xOverlap = (BIRD_X + BIRD_R > p.x) && (BIRD_X - BIRD_R < p.x + PIPE_W);
        if (xOverlap) {
            if (birdY - BIRD_R < gapTop || birdY + BIRD_R > gapBot) {
                dead = true;
            }
        }

        render_pipe_delta(p);

        if (p.x + PIPE_W < 0) {
            // pipe scrolled off the left edge - recycle it as a new pipe past the last one
            float maxX = p.x;
            for (int j = 0; j < pipeCount; j++) if (pipes[j].x > maxX) maxX = pipes[j].x;
            spawn_pipe(maxX + PIPE_SPACING);
            pipes[i] = pipes[pipeCount - 1];
            pipeCount--;
            i--;
        }
    }

    draw_ground();
    draw_bird();
    draw_hud(false);

    if (dead) {
        state = GameState::DEAD;
        if (score > bestScore) bestScore = score;
        draw_dead_overlay();
    }
}

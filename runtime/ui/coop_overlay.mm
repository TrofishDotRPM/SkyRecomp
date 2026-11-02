// Co-op overlay: a marker over the other player, and popups, bottom right: what another player's progress just gave this one. They appear above the game's own HUD,
// take no input, and fade out by themselves. The list lives in runtime/hle/coop.c.
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

extern "C" int coop_toasts(char (*text)[64], float *age, int max);
extern "C" const char *coop_other_name(void);
extern "C" bool coop_marker_target(float out[3]);
extern "C" void coop_marker_view(const float world[3], float view[3], float *proj_x, float *proj_y);

// The colours of the HUD's own button labels, sampled from "Help": a gold fill, outlined in dark brown.
static void label_text(ImDrawList *dl, ImFont *font, float size, ImVec2 at, const char *text, float alpha, float u) {
    const float o = 1.4f * u;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) dl->AddText(font, size, ImVec2(at.x + dx * o, at.y + dy * o), IM_COL32(80, 72, 48, (int)(255 * alpha)), text);
    dl->AddText(font, size, at, IM_COL32(232, 208, 120, (int)(255 * alpha)), text);
}

// Over the other player when on screen; otherwise an arrow at the edge of the screen, pointing their way. Seen through
// walls: it's drawn over the finished picture.
static void marker_draw(ImVec2 display, ImFont *font, float u) {  // (font: the label's)
    static float shown[3];
    static double last;
    static bool have;
    float target[3], view[3], px, py;
    if (!coop_marker_target(target)) { have = false; return; }
    double now = ImGui::GetTime();
    float dist2 = 0;
    for (int i = 0; i < 3; i++) dist2 += (target[i] - shown[i]) * (target[i] - shown[i]);
    if (!have || dist2 > 1000 * 1000) {  // (the first sight, or a jump: no gliding across the map)
        memcpy(shown, target, sizeof shown);
    } else {
        float k = 1 - exp(-(float)(now - last) * 15);  // (updates come at 20 Hz; the picture is smoother)
        for (int i = 0; i < 3; i++) shown[i] += (target[i] - shown[i]) * k;
    }
    last = now;
    have = true;
    coop_marker_view(shown, view, &px, &py);
    if (!(px > 0.1f && px < 10 && py > 0.1f && py < 10)) return;  // (no 3D camera now)

    bool behind = view[2] > -1;
    float depth = behind ? 1 : -view[2];
    float nx = view[0] * px / depth, ny = view[1] * py / depth;
    if (behind) {  // past the edge, on the side that turns the camera towards them soonest
        float len = sqrtf(view[0] * view[0] + view[1] * view[1]);
        nx = len > 0.01f ? view[0] / len * 4 : 0, ny = len > 0.01f ? view[1] / len * 4 : -4;
    }
    float mx = 40 * u, my = 48 * u;
    // The picture is a box in the window, with bars if their shapes differ; its shape is the projection's
    float aspect = py / px, bw = fminf(display.x, display.y * aspect), bh = bw / aspect;
    float ox = (display.x - bw) / 2, oy = (display.y - bh) / 2;
    float sx = ox + (nx + 1) / 2 * bw, sy = oy + (1 - ny) / 2 * bh;
    bool edge = behind || sx < ox + mx || sx > ox + bw - mx || sy < oy + my || sy > oy + bh - my;
    if (getenv("LARECOMP_COOP_TEST")) {  // (what the marker is really working from)
        static double logged;
        if (now - logged > 1.0) {
            logged = now;
            fprintf(stderr, "[coop] marker: display %.0fx%.0f target %.0f %.0f %.0f shown %.0f %.0f %.0f view %.0f %.0f %.0f proj %.3f %.3f ndc %.3f %.3f screen %.0f %.0f edge %d\n",
                    display.x, display.y, target[0], target[1], target[2], shown[0], shown[1], shown[2], view[0], view[1], view[2],
                    px, py, nx, ny, sx, sy, (int)edge);
        }
    }
    ImVec2 dir(0, 1);  // where the point of the arrow aims: down, at them
    if (edge) {
        float cx = display.x / 2, cy = display.y / 2, dx = sx - cx, dy = sy - cy;
        float s = fminf(dx != 0 ? (bw / 2 - mx) / fabsf(dx) : 1e9f, dy != 0 ? (bh / 2 - my) / fabsf(dy) : 1e9f);
        sx = cx + dx * s, sy = cy + dy * s;
        float len = sqrtf(dx * dx + dy * dy);
        dir = len > 0.01f ? ImVec2(dx / len, dy / len) : ImVec2(0, 1);
    }
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const ImU32 fill = IM_COL32(232, 208, 120, 255), line = IM_COL32(80, 72, 48, 255);  // (as the label)
    float h = 9 * u, w = 6 * u;
    ImVec2 side(-dir.y, dir.x);
    ImVec2 tip(sx + dir.x * h, sy + dir.y * h), a(sx - dir.x * h * 0.3f + side.x * w, sy - dir.y * h * 0.3f + side.y * w),
        b(sx - dir.x * h * 0.3f - side.x * w, sy - dir.y * h * 0.3f - side.y * w);
    dl->AddTriangleFilled(tip, b, a, fill);
    dl->AddTriangle(tip, b, a, line, 1.5f * u);
    char label[64];
    float real = sqrtf(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    snprintf(label, sizeof label, "%s  %dm", coop_other_name(), (int)(real / 100));  // (the game's units are centimetres)
    float size = 18 * u;
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, label);
    float tx = sx - ts.x / 2, ty = sy - h - ts.y - 4 * u;
    tx = fminf(fmaxf(tx, ox + 8 * u), ox + bw - ts.x - 8 * u), ty = fminf(fmaxf(ty, oy + 8 * u), oy + bh - ts.y - 8 * u);
    label_text(dl, font, size, ImVec2(tx, ty), label, 1, u);
}

extern "C" void coop_overlay_draw(ImVec2 display, ImFont *font, ImFont *label_font) {
    char text[4][64];
    float age[4];
    int n = coop_toasts(text, age, 4);
    // Sized to the picture, not the window (16:9, as the game's own HUD)
    float bw = fminf(display.x, display.y * 16.0f / 9.0f), bh = bw * 9.0f / 16.0f, ox = (display.x - bw) / 2, oy = (display.y - bh) / 2;
    float u = bh / 720.0f, size = 20 * u, pad = 8 * u, gap = 6 * u;
    marker_draw(display, label_font, u);
    float y = oy + bh - 24 * u;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    for (int i = n - 1; i >= 0; i--) {  // (the newest at the bottom)
        float a = age[i] > 0.8f ? (1 - age[i]) / 0.2f : 1;  // the last part of its life fades
        char line[128];
        snprintf(line, sizeof line, "%s found: %s", coop_other_name(), text[i]);
        ImVec2 ts = label_font->CalcTextSizeA(size, FLT_MAX, 0, line);
        ImVec2 hi(ox + bw - 24 * u, y), lo(hi.x - ts.x - 2 * pad, y - ts.y - 2 * pad);
        // (a drop shadow, below and to the right)
        ImVec2 at(lo.x + pad, lo.y + pad);
        const float o = 1.6f * u;
        dl->AddText(label_font, size, ImVec2(at.x + o, at.y + o), IM_COL32(80, 72, 48, (int)(255 * a)), line);
        dl->AddText(label_font, size, ImVec2(at.x + o, at.y), IM_COL32(80, 72, 48, (int)(160 * a)), line);
        dl->AddText(label_font, size, ImVec2(at.x, at.y + o), IM_COL32(80, 72, 48, (int)(160 * a)), line);
        dl->AddText(label_font, size, at, IM_COL32(232, 208, 120, (int)(255 * a)), line);
        y = lo.y - gap;
    }
}

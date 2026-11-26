// Widescreen HUD aspect correction (SFA-NX); see port/include/fh_hud_aspect.h for what it does and why.
#include "fh_hud_aspect.h"
#include "foxhollow_config.h"

#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <stdio.h>

// Game symbols (game/include/track/intersect_hud.h, main/lightmap.h, main/textrender.h), declared here so
// this port file needs no game headers.
extern f32 hudMatrix[4][4];
int isWidescreen(void);
extern int lbl_803DC9C8; // gGameTextCommandCount: text commands queued this frame

// Corrected width: a 4:3 layout on a 16:9 frame is 3/4 as wide.
#define FH_HUD_SCALE 0.75f
// Quads at least this wide (640-space pixels) are full-screen layers: never corrected.
#define FH_HUD_FULL_WIDTH 600.0f
// Text command slots (GameTextRuntime.commands holds 0x80)
#define FH_HUD_TEXT_SLOTS 0x80
// Frames between [hud] log lines (30 s at 60 fps)
#define FH_HUD_LOG_FRAMES 1800
#ifdef __SWITCH__
extern int sfa_log_extra_on; /* switch/src/log_switch.cpp: the - menu's Extra logging */
static int fh_hud_extra_logging(void) { return __atomic_load_n(&sfa_log_extra_on, __ATOMIC_RELAXED); }
#else
static int fh_hud_extra_logging(void) { return 1; } /* other builds keep the statistic */
#endif

static int sEnabled = 1;
static int sStack[16];
static int sDepth;
// Anchor recorded per queued text command, and how many slots are filled so far this frame.
static u8 sTextAnchor[FH_HUD_TEXT_SLOTS];
static int sTextFilled;
// Log counters: quads drawn per anchor (index FH_HUD_NONE counts in-scope full-width quads kept
// stretched), frames counted, and the last active state logged (-1 none yet).
static unsigned sQuads[FH_HUD_LEFT_CLIPPED + 1];
static unsigned sFrames;
static int sLoggedActive = -1;

void fhHudAspectSetEnabled(int on) {
  sEnabled = on != 0;
  printf("[hud] widescreen HUD: %s\n", sEnabled ? "corrected" : "stretched (original)");
}

int fhHudAspectEnabled(void) { return sEnabled; }

// The frame is 16:9 when the screen style is forced wide (FOXHOLLOW_SCREEN_STYLE), or, when it is not forced,
// when the game's own Widescreen option is on. The forced case can't use isWidescreen(): map-block init
// (lightmap_initmapblocks.c) clears every render flag, and only loading a save sets the flag again.
static int frame_is_wide(void) {
  return fhConfigScreenStyleIsForced() ? fhConfigScreenStyleIsWide() : isWidescreen() != 0;
}

int fhHudAspectActive(void) { return sEnabled && frame_is_wide(); }

// The anchor quads use now: the innermost scope's, none outside every scope.
static int current_anchor(void) { return sDepth > 0 ? sStack[sDepth - 1] : FH_HUD_NONE; }

// The anchor change also ends the run of text commands queued under the old anchor, so record it first.
void fhHudPush(int anchor) {
  fhHudTextRecord(lbl_803DC9C8);
  if (sDepth < (int)(sizeof(sStack) / sizeof(sStack[0]))) {
    sStack[sDepth] = anchor;
  }
  ++sDepth;
}

void fhHudPop(void) {
  fhHudTextRecord(lbl_803DC9C8);
  if (sDepth > 0) {
    --sDepth;
  }
}

// The fixed point of the correction in clip space (-1 left edge, 0 centre, +1 right edge); for "own" the
// clip position of the quad's centre cx (640-space pixels) under hudMatrix.
static f32 fixed_point(int anchor, f32 cx) {
  switch (anchor) {
  case FH_HUD_LEFT:
  case FH_HUD_LEFT_CLIPPED: // same fixed point as left
    return -1.0f;
  case FH_HUD_RIGHT:
    return 1.0f;
  case FH_HUD_OWN:
    return hudMatrix[0][0] * cx + hudMatrix[0][3];
  default:
    return 0.0f;
  }
}

void fhHudSetProjection(f32 x0, f32 x1) {
  const int anchor = current_anchor();
  const f32 width = x1 > x0 ? x1 - x0 : x0 - x1;
  Mtx44 m;
  f32 f;
  int i, j;
  if (anchor == FH_HUD_NONE || !fhHudAspectActive()) {
    GXSetProjection(hudMatrix, GX_ORTHOGRAPHIC);
    return;
  }
  // Full-width layers keep the original mapping, and so do quads entirely off the 640-unit screen: menus park
  // elements there to hide them (the title's START panel waits at x < 0), and the narrower corrected mapping
  // would pull them into view at the screen's edge.
  // (Clipped panels skip this test: their content is cut to a corrected scissor box whatever its width.)
  if (anchor != FH_HUD_LEFT_CLIPPED &&
      (width >= FH_HUD_FULL_WIDTH || (x0 <= 0.0f && x1 <= 0.0f) || (x0 >= 640.0f && x1 >= 640.0f))) {
    ++sQuads[FH_HUD_NONE];
    GXSetProjection(hudMatrix, GX_ORTHOGRAPHIC);
    return;
  }
  ++sQuads[anchor == FH_HUD_LEFT_CLIPPED ? FH_HUD_LEFT : anchor]; // clipped counts as left in the log
  // clip x' = A * clip x + (1 - A) * F: 3/4 as wide around the fixed point F; y and z unchanged.
  for (i = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j) {
      m[i][j] = hudMatrix[i][j];
    }
  }
  f = fixed_point(anchor, 0.5f * (x0 + x1));
  for (j = 0; j < 4; ++j) {
    m[0][j] = FH_HUD_SCALE * hudMatrix[0][j];
  }
  m[0][3] += (1.0f - FH_HUD_SCALE) * f;
  GXSetProjection(m, GX_ORTHOGRAPHIC);
}

// The same correction as fhHudSetProjection, on one 640-space position (hudMatrix spans 0..640 in x).
f32 fhHudMapX(f32 x) {
  const int anchor = current_anchor();
  f32 clip;
  if (anchor == FH_HUD_NONE || anchor == FH_HUD_OWN || !fhHudAspectActive()) {
    return x;
  }
  clip = x / 320.0f - 1.0f;
  clip = FH_HUD_SCALE * clip + (1.0f - FH_HUD_SCALE) * fixed_point(anchor, x);
  return (clip + 1.0f) * 320.0f;
}

void fhHudScissor(u32 x, u32 y, u32 w, u32 h) {
  const f32 x0 = fhHudMapX((f32)x);
  const f32 x1 = fhHudMapX((f32)(x + w));
  const u32 left = (u32)(x0 + 0.5f);
  GXSetScissor(left, y, (u32)(x1 + 0.5f) - left, h);
}

void fhHudFrameTick(void) {
  const int active = fhHudAspectActive();
  if (active != sLoggedActive) {
    printf("[hud] widescreen HUD correction %s (menu %s, game %s)\n", active ? "active" : "inactive",
           sEnabled ? "corrected" : "stretched", frame_is_wide() ? "16:9" : "4:3");
    sLoggedActive = active;
  }
  if (++sFrames >= FH_HUD_LOG_FRAMES) {
    /* The per-frame quad counts are a testing statistic: on Switch only with the - menu's Extra logging
       (switch/src/log_switch.h). */
    if (active && fh_hud_extra_logging()) {
      const f32 n = (f32)sFrames;
      printf("[hud] per frame: %.1f left, %.1f centre, %.1f right, %.1f own quads corrected; %.1f full-width or "
             "off-screen kept\n",
             sQuads[FH_HUD_LEFT] / n, sQuads[FH_HUD_CENTRE] / n, sQuads[FH_HUD_RIGHT] / n, sQuads[FH_HUD_OWN] / n,
             sQuads[FH_HUD_NONE] / n);
    }
    sFrames = 0;
    sQuads[0] = sQuads[1] = sQuads[2] = sQuads[3] = sQuads[4] = 0;
  }
}

// Text commands queued outside any scope are UI text all the same: they get the centre anchor.
void fhHudTextRecord(int queuedCount) {
  int anchor = current_anchor();
  if (anchor == FH_HUD_NONE) {
    anchor = FH_HUD_CENTRE;
  }
  if (queuedCount < sTextFilled) {
    sTextFilled = 0; // the queue was replayed and emptied since the last record
  }
  if (queuedCount > FH_HUD_TEXT_SLOTS) {
    queuedCount = FH_HUD_TEXT_SLOTS;
  }
  while (sTextFilled < queuedCount) {
    sTextAnchor[sTextFilled++] = (u8)anchor;
  }
}

int fhHudTextAnchor(int index) {
  if (index < 0 || index >= sTextFilled) {
    return FH_HUD_CENTRE;
  }
  return sTextAnchor[index];
}

// Viewport saved by fhHudWorldInsetBegin for fhHudWorldInsetEnd; sInsetActive when it must be restored.
static f32 sInsetViewport[6];
static int sInsetActive;
// Last effective camera aspect logged by fhHudWorldInsetBegin (0 none yet), so it is logged once per change.
static f32 sInsetLoggedAspect;

// A 3D model drawn into a HUD box from the main scene (the world map's comms portrait, object 467): the box
// moves with the centre-anchored HUD, but the model is projected by the scene camera and stays where the 4:3
// layout put it (hardware, build 1834: with the camera already 16:9 the head sat at the box's old centre).
// So move the viewport by the box centre's shift (like the in-flight comms head, dlls/engine/0 headDisplayDraw),
// and scale it around the box centre by k = 0.75 * A / (4/3) with the camera aspect A, which is 1 for a 16:9
// camera and squeezes a 4:3 camera's (stretched) model to the corrected width.
void fhHudWorldInsetBegin(u32 x, u32 y, u32 w, u32 h) {
  f32 proj[7];
  f32 aspect;
  f32 k = 1.0f;
  f32 cx;
  f32 shift;
  sInsetActive = 0;
  if (!fhHudAspectActive()) {
    GXSetScissor(x, y, w, h);
    return;
  }
  // The scissor box and its centre follow the centre-anchored frame around it.
  cx = (f32)x + 0.5f * (f32)w;
  fhHudPush(FH_HUD_CENTRE);
  fhHudScissor(x, y, w, h);
  shift = fhHudMapX(cx) - cx;
  fhHudPop();
  // Effective aspect from the loaded perspective projection: m00 = m11 / A (proj = type, m00, m02, m11, ...).
  GXGetProjectionv(proj);
  aspect = 0.0f;
  if (proj[0] == (f32)GX_PERSPECTIVE && proj[1] > 0.0f && proj[3] > 0.0f) {
    aspect = proj[3] / proj[1];
    k = FH_HUD_SCALE * aspect * 0.75f; // 0.75 * A / (4/3)
    if (k > 0.98f && k < 1.02f) {
      k = 1.0f; // a 16:9 camera: move only
    }
  }
  if (aspect < sInsetLoggedAspect - 0.01f || aspect > sInsetLoggedAspect + 0.01f || sInsetLoggedAspect == 0.0f) {
    printf("[hud] world inset: camera aspect %.3f, viewport scale %.3f, shift %.1f\n", aspect, k, shift);
    sInsetLoggedAspect = aspect != 0.0f ? aspect : -1.0f;
  }
  GXGetViewportv(sInsetViewport);
  // Scale around the box centre by k, then move the box centre by shift.
  GXSetViewport(cx + shift + (sInsetViewport[0] - cx) * k, sInsetViewport[1], sInsetViewport[2] * k,
                sInsetViewport[3], sInsetViewport[4], sInsetViewport[5]);
  sInsetActive = 1;
}

// Restores the viewport fhHudWorldInsetBegin moved (the caller restores the scissor itself).
void fhHudWorldInsetEnd(void) {
  if (sInsetActive) {
    GXSetViewport(sInsetViewport[0], sInsetViewport[1], sInsetViewport[2], sInsetViewport[3], sInsetViewport[4],
                  sInsetViewport[5]);
    sInsetActive = 0;
  }
}

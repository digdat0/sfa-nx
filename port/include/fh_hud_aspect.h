#ifndef FH_HUD_ASPECT_H_
#define FH_HUD_ASPECT_H_

// Widescreen HUD aspect correction (SFA-NX).
//
// The game lays out all 2D UI in a 640x480 space (hudMatrix, an orthographic projection). In the game's
// 16:9 mode only the 3D camera widens: the frame is 16:9 but those 640 units still span its full width,
// so every icon, letter and box is drawn 4/3 too wide (the GameCube's 16:9 mode was anamorphic, made for
// TVs that stretch the picture). This module draws UI quads with a corrected projection instead: 3/4 as
// wide, pinned to a fixed point so the element keeps its place:
//   left   - the screen's left edge stays put (top-left panels stay in the corner)
//   centre - the screen centre stays put (dialogue, menus, prompts)
//   right  - the screen's right edge stays put (buttons, C-menu, counters)
//   own    - the quad's own centre stays put (sprites placed on 3D positions, like the aim reticle)
// Correction only applies inside a scope opened with fhHudPush (the UI draw pass, and the text pass per
// queued command), so everything else drawn with hudMatrix (screen filters, fades, sprites drawn into the
// EFB for texture copies) keeps the original mapping. Quads 600 px or wider are full-screen layers and
// are never corrected either.
#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  FH_HUD_NONE = 0, // original stretched mapping
  FH_HUD_LEFT,
  FH_HUD_CENTRE,
  FH_HUD_RIGHT,
  FH_HUD_OWN,
};

// Menu toggle (switch_menu.ini hud_aspect); on by default. Takes effect only in the game's 16:9 mode.
void fhHudAspectSetEnabled(int on);
int fhHudAspectEnabled(void);
// Enabled and the game is in 16:9 mode
int fhHudAspectActive(void);

// Anchor scopes (nest; the innermost wins). Outside every scope quads keep the original mapping.
void fhHudPush(int anchor);
void fhHudPop(void);

// Loads the HUD projection for a quad spanning x0..x1 (640-space pixels): hudMatrix, or the corrected
// matrix for the current anchor.
void fhHudSetProjection(f32 x0, f32 x1);
// Where 640-space x lands, in the same units, under the current anchor (unchanged when inactive). Used to
// move 3D HUD overlays (their viewport centre) and scissor boxes with the sprites around them.
f32 fhHudMapX(f32 x);
// GXSetScissor with its x range moved by fhHudMapX
void fhHudScissor(u32 x, u32 y, u32 w, u32 h);

// Once per frame, at the UI draw pass: counts frames for the periodic [hud] log line
void fhHudFrameTick(void);

// Text pass: the game queues text commands while drawing the UI and replays them later (gameTextRun).
// fhHudTextRecord stores the current anchor for the commands queued so far (call before the anchor
// changes and before the replay); fhHudTextAnchor gives the anchor recorded for command i.
void fhHudTextRecord(int queuedCount);
int fhHudTextAnchor(int index);

#ifdef __cplusplus
}
#endif

#endif /* FH_HUD_ASPECT_H_ */

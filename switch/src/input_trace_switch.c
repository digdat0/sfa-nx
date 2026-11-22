// Diagnostic: logs port 0's pad state whenever it changes ("[input] ..." lines in stdout.log).
// Wraps Aurora's PADRead (-Wl,--wrap=PADRead). The game's pad update calls PADRead first, so the
// game-side values printed here (held/pressed/mask/disable flags, player state) are the ones left by
// the previous frame -- i.e. what gameplay code saw and consumed (a button gameplay acted on is
// usually cleared from mask via buttonDisable). Remove once input on Switch is confirmed.
// The wrapper must stay even then: it is also where the - menu filters the pads (sfaSwitchMenuFilterPad
// below, for ZL/ZR-bound buttons and blanking input while the menu is open). Logging stops after 6000
// lines, so a long session cannot fill the log.
// Built with the game's include dirs (CMakeLists.txt).
#include <stdio.h>
#include "main/dll/player_state.h"
#include "main/objanim_internal.h"
#include "main/objseq.h"
#include "sys/objects.h"
#include "main/frame_timing.h"
#include "main/lightmap.h"
#include "main/model_engine_ui.h"

// Only the fields read here; Aurora's full PADStatus is larger, but the game passes Aurora's array.
typedef struct {
    u16 button;
    s8 stickX, stickY, substickX, substickY;
    u8 triggerLeft, triggerRight, analogA, analogB;
    s8 err;
} PadStatusHead; // leading fields of Aurora's PADStatus

extern u32 __real_PADRead(void* status);
extern void sfaSwitchMenuFilterPad(void* status); // switch_menu.cpp: ZL/ZR-bound buttons, mapping tester, no input while the - menu is open
_Static_assert(sizeof(PadStatusHead) == 12, "leading PADStatus fields (the full struct also has extButton)");
extern s32 PADGetIndexForPort(u32 port);

// Game state that decides whether gameplay acts on input (decomp names).
extern u8 joypadDisabled;
extern u8 gDvdErrorPauseActive;
extern s8 gNpcDialogueActive;
extern u32 gPadButtonMask[4];
extern u32 gPadButtonsHeld[4];
extern u32 gPadButtonsJustPressed[4];

u32 __wrap_PADRead(void* status) {
    static u64 calls;
    static unsigned lines;
    static u32 lastKey[18];
    // previous frame's state, read before this frame's pad update overwrites it
    const u32 pressed = gPadButtonsJustPressed[0], held = gPadButtonsHeld[0], mask = gPadButtonMask[0];
    GameObject* player = Obj_GetPlayerObject();
    const PlayerState* ps = player != NULL ? (const PlayerState*)player->extra : NULL;
    const int seq = getCurSeqNo();
    const int anim = ps != NULL ? ps->curAnimId : -1;
    const u32 flags360 = ps != NULL ? ps->flags360 : 0;
    const int character = ps != NULL ? ps->characterId : -2;
    const int pjp = ps != NULL ? ps->buttonsJustPressed : -1;
    const int def = player != NULL ? player->anim.romDefNo : -1;
    const int move = player != NULL ? player->anim.currentMove : -1;
    // playerUpdate's early exits (cutscene timer, UI dll 4, out of map bounds) skip playerDoControls
    const int cut6 = ps != NULL && ps->cutsceneTimer >= 6.0f;
    const int uiDll = getCurUiDll();
    const int hasParent = player != NULL && player->anim.parent != NULL;
    const int hasFocus = ps != NULL && ps->focusObject != NULL;
    const int inBounds = player != NULL ? isInBounds(player->anim.localPosX, player->anim.localPosZ) : -9;
    const int ctl = ps != NULL ? ps->baddie.controlMode : -1;

    const u32 rc = __real_PADRead(status);
    ++calls;
    const PadStatusHead* p = (const PadStatusHead*)status;
        // Everything that is logged; a line is printed only when one of these changed (or a button went
        // down), so the log shows state changes instead of one line per frame.
    const u32 key[18] = {p->button, (u32)(u8)p->err, joypadDisabled, gDvdErrorPauseActive,
                         (u32)(u8)gNpcDialogueActive, mask, pressed, (u32)seq, (u32)anim, flags360 & 0x200000,
                         (u32)def, (u32)PADGetIndexForPort(0),
                         (u32)cut6, (u32)uiDll, (u32)hasParent, (u32)hasFocus, (u32)inBounds, (u32)ctl};
    int changed = pressed != 0;
    for (int i = 0; i < 18; ++i) {
        changed |= key[i] != lastKey[i];
        lastKey[i] = key[i];
    }
    if (player != NULL && calls % 600 == 0) changed = 1; // periodic snapshot while a player exists
    // SFA-NX: the trace lines are off by default (~6k lines a session); build with -DSFA_NX_INPUT_TRACE
    // to print them again. The state is still tracked so turning it on needs no other change.
#ifdef SFA_NX_INPUT_TRACE
    if (changed && lines < 6000) {
        ++lines;
        printf("[input] call %llu raw 0x%04x stick %d,%d | held 0x%x pressed 0x%x mask 0x%x jd %u dvd %u npc %d "
               "| seq %d player def 0x%x move 0x%x anim 0x%x char %d f360 0x%08x pjp 0x%x | sdl %d "
               "| cut %d (%.1f) ui %d parent %d focus %d inb %d ctl 0x%x pos %.1f,%.1f dt %.3f\n",
               (unsigned long long)calls, p->button, p->stickX, p->stickY, held, pressed, mask, joypadDisabled,
               gDvdErrorPauseActive, gNpcDialogueActive, seq, def, move, anim, character, flags360, pjp,
               (int)key[11], cut6, ps != NULL ? ps->cutsceneTimer : 0.0f, uiDll, hasParent, hasFocus, inBounds,
               ctl, player != NULL ? player->anim.localPosX : 0.0f, player != NULL ? player->anim.localPosZ : 0.0f,
               timeDelta);
    }
#else
    (void)changed;
    (void)lines;
#endif
    sfaSwitchMenuFilterPad(status);
    return rc;
}

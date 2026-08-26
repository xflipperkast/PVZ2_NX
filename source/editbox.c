/* editbox.c -- Switch software keyboard backing the engine's EditBox/TextBox
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <switch.h>
#include <string.h>
#include <stdio.h>

#include "editbox.h"
#include "util.h"

extern void watchdog_set_suspended(int suspended);

static Mutex g_lock;
static int   g_inited = 0;
static int   g_open = 0;
static char  g_text[1024] = "";

static void ensure_init(void) {
  if (!g_inited) { mutexInit(&g_lock); g_inited = 1; }
}

// Blocking. The engine calls ShowEditBox, then polls IsOpenEditBox every frame
// and reads GetEditBoxText when it closes. The Switch keyboard is a full-screen
// modal, so we run it inline: the caller's frame pauses while the keyboard is
// up (the engine thread is blocked here), and by the time we return the text is
// ready and IsOpenEditBox reads back closed. BGM keeps playing on the audio
// thread. swkbdShow pumps the applet loop internally.
void editbox_show_prompt(const char *initial, int maxlen, const char *header,
                         const char *subtext, const char *guide) {
  ensure_init();

  mutexLock(&g_lock);
  if (g_open) { mutexUnlock(&g_lock); return; } // re-entrant guard
  g_open = 1;
  mutexUnlock(&g_lock);

  if (maxlen <= 0 || maxlen > 1000) maxlen = 32;

  char out[1024] = "";
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_SUCCEEDED(rc)) {
    swkbdConfigMakePresetDefault(&kbd);
    if (initial && initial[0]) swkbdConfigSetInitialText(&kbd, initial);
    if (header && header[0]) swkbdConfigSetHeaderText(&kbd, header);
    if (subtext && subtext[0]) swkbdConfigSetSubText(&kbd, subtext);
    if (guide && guide[0]) swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetStringLenMax(&kbd, (u32)maxlen);
    swkbdConfigSetBlurBackground(&kbd, true);
    watchdog_set_suspended(1);
    rc = swkbdShow(&kbd, out, sizeof(out));
    watchdog_set_suspended(0);
    swkbdClose(&kbd);
    if (R_FAILED(rc))                                   // cancelled: keep initial
      snprintf(out, sizeof(out), "%s", initial ? initial : "");
  } else {
    debugPrintf("editbox: swkbdCreate failed rc=0x%x\n", rc);
    snprintf(out, sizeof(out), "%s", initial ? initial : "");
  }

  // defensive byte cap (swkbd limits by codepoints; a UTF-8 name stays short)
  if ((int)strlen(out) > maxlen * 4) out[maxlen * 4] = '\0';

  mutexLock(&g_lock);
  snprintf(g_text, sizeof(g_text), "%s", out);
  g_open = 0;
  mutexUnlock(&g_lock);
}

void editbox_show(const char *initial, int maxlen) {
  editbox_show_prompt(initial, maxlen, NULL, NULL, NULL);
}

int editbox_is_open(void) {
  if (!g_inited) return 0;
  mutexLock(&g_lock);
  const int o = g_open;
  mutexUnlock(&g_lock);
  return o;
}

const char *editbox_text(void) {
  return g_text; // stable storage; the engine copies it via GetStringUTFRegion
}

void editbox_close(void) {
  ensure_init();
  mutexLock(&g_lock);
  g_open = 0;
  mutexUnlock(&g_lock);
}

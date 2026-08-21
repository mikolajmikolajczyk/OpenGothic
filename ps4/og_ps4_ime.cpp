#include "og_ps4_ime.h"

#include "ps4_app.h"

#include <orbis/libkernel.h>
#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include <cstdint>
#include <cstring>

namespace Ps4Og {

namespace {

enum : uint32_t { MaxText = 64 };

// The dialog is handed a POINTER to these and outlives the call that raised it, so they are file
// static rather than locals of imeBegin.
char16_t g_text[MaxText]  = {};
char16_t g_title[]        = u"Savegame name";
char16_t g_holder[]       = u"name this save";

bool     g_ready   = false;
bool     g_up      = false;
// Consecutive polls that saw ORBIS_DIALOG_STATUS_NONE. See imePoll.
uint32_t g_noneRuns = 0;
enum : uint32_t { kNoneGiveUp = 360 };   // ~6 s at 60 Hz
uint32_t g_opened  = 0;      // how many panels this process has raised
int32_t  g_user    = int32_t(ORBIS_USER_SERVICE_USER_ID_SYSTEM);

// UTF-8 in, UTF-16 out. Gothic's save names are the characters SavNameDialog itself accepts -
// a-z, A-Z, 0-9, space and '.' - so anything above the BMP cannot arrive; the multi-byte arms are
// here because a name loaded from an existing save could carry Latin-1 accents through the ini.
void toUtf16(const std::string& in, char16_t* out, size_t outCount) {
  size_t o = 0;
  for(size_t i=0; i<in.size() && o+1<outCount; ) {
    const uint8_t c = uint8_t(in[i]);
    if(c<0x80) {
      out[o++] = char16_t(c);
      i += 1;
      } else
    if((c & 0xE0)==0xC0 && i+1<in.size()) {
      out[o++] = char16_t(((c & 0x1F)<<6) | (uint8_t(in[i+1]) & 0x3F));
      i += 2;
      } else
    if((c & 0xF0)==0xE0 && i+2<in.size()) {
      out[o++] = char16_t(((c & 0x0F)<<12) | ((uint8_t(in[i+1]) & 0x3F)<<6) |
                          (uint8_t(in[i+2]) & 0x3F));
      i += 3;
      } else {
      // Not decodable, and skipped rather than substituted: a name that comes back with a
      // replacement character in it would look like the panel did that.
      i += 1;
      }
    }
  out[o] = 0;
  }

// Bounded by MaxText as well as by the terminator: `in` is g_text, which the system framework
// fills, and a buffer this side owns should not be walked on the framework's promise to end it.
std::string toUtf8(const char16_t* in) {
  std::string out;
  for(size_t i=0; i<MaxText && in[i]!=0; ++i) {
    const uint32_t c = uint32_t(uint16_t(in[i]));
    if(c>=0xD800 && c<=0xDFFF)
      continue;               // BMP only; see the header
    if(c<0x80) {
      out.push_back(char(c));
      } else
    if(c<0x800) {
      out.push_back(char(0xC0 | (c>>6)));
      out.push_back(char(0x80 | (c & 0x3F)));
      } else {
      out.push_back(char(0xE0 | (c>>12)));
      out.push_back(char(0x80 | ((c>>6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
      }
    }
  return out;
  }

int32_t rawInit() {
  OrbisImeDialogSetting p = {};
  p.userId              = uint32_t(g_user);
  p.type                = ORBIS_TYPE_BASIC_LATIN;
  p.supportedLanguages  = 0;    // measured: ~0 is refused, 0 is the accepted "unspecified"
  p.enterLabel          = ORBIS_BUTTON_LABEL_DEFAULT;
  p.inputMethod         = ORBIS__DEFAULT;
  p.filter              = nullptr;
  p.option              = 0;
  p.maxTextLength       = MaxText-1;
  p.inputTextBuffer     = reinterpret_cast<wchar_t*>(g_text);
  // 0,0 with LEFT/TOP: measured to work, and it is the only placement that depends on no
  // coordinate convention. 960,540 with CENTER also works and would break if the space ever
  // stopped being device pixels.
  p.posx                = 0.f;
  p.posy                = 0.f;
  p.horizontalAlignment = ORBIS_H_LEFT;
  p.verticalAlignment   = ORBIS_V_TOP;
  p.placeholder         = reinterpret_cast<wchar_t*>(g_holder);
  p.title               = reinterpret_cast<wchar_t*>(g_title);
  return sceImeDialogInit(&p,nullptr);
  }

// THE TEARDOWN LADDER, and what it turned out to prove. It was built because a standalone ime probe measured a
// second Init refused with 0x80bc0008 whatever its parameters - and in the game ATTEMPT 0, the
// plain retry, succeeded. The demo re-opened in the same frame as Term; a player names their
// second save tens of seconds later. So the refusal was a settling time, not a per-process limit
// (og_ps4_ime.h has the retraction).
//
// It stays. It costs one extra Init on a path a human drives, it names whichever sequence works,
// and it is what would catch the restriction returning on another firmware.
const char* tryTeardown(uint32_t attempt) {
  switch(attempt) {
    case 0:
      return "nothing - a plain retry, which is the baseline";
    case 1: {
      sceImeDialogForceClose();
      const int32_t t = sceImeDialogTerm();
      ps4_log("ime: teardown 1: ForceClose then Term -> 0x%08x",uint32_t(t));
      return "ForceClose + Term";
      }
    case 2: {
      const int32_t t = sceImeDialogTerm();
      ps4_log("ime: teardown 2: a second Term -> 0x%08x",uint32_t(t));
      return "Term twice";
      }
    case 3: {
      const int32_t u = sceSysmoduleUnloadModule(ORBIS_SYSMODULE_IME_DIALOG);
      const int32_t l = sceSysmoduleLoadModule  (ORBIS_SYSMODULE_IME_DIALOG);
      ps4_log("ime: teardown 3: unload+load IME_DIALOG -> 0x%08x / 0x%08x",
              uint32_t(u),uint32_t(l));
      return "module unload + load";
      }
    }
  return "?";
  }

}

bool imeInit() {
  if(g_ready)
    return true;

  const int32_t userRc = sceUserServiceInitialize(nullptr);
  int32_t       user    = ORBIS_USER_SERVICE_USER_ID_INVALID;
  const int32_t iuRc    = sceUserServiceGetInitialUser(&user);
  if(iuRc==0 && user!=ORBIS_USER_SERVICE_USER_ID_INVALID)
    g_user = user;

  const int32_t m1 = sceSysmoduleLoadModule(ORBIS_SYSMODULE_LIBIME);
  const int32_t m2 = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
  const int32_t cd = sceCommonDialogInitialize();

  // One line with every code, because if the panel never appears this is the only place that says
  // which of the five steps refused.
  ps4_log("ime: init userService=0x%08x initialUser=0x%08x(id %d) libime=0x%08x imeDialog=0x%08x "
          "commonDialog=0x%08x",
          uint32_t(userRc),uint32_t(iuRc),int(g_user),uint32_t(m1),uint32_t(m2),uint32_t(cd));

  g_ready = (m2==0);
  if(!g_ready)
    ps4_log("ime: the IME_DIALOG sysmodule did not load - savegame names cannot be typed on this "
            "system, and the save itself is unaffected");
  return g_ready;
  }

bool imeBegin(const std::string& initial) {
  if(!imeInit())
    return false;
  if(g_up)
    return true;   // already raised; the caller polls

  toUtf16(initial,g_text,MaxText);

  // First panel of the process: a plain Init, which the ime probe measured as always working.
  //
  // ⚠ AND IF IT DOES NOT, FALL THROUGH TO THE LADDER RATHER THAN GIVING UP. This used to `return
  // false` without incrementing g_opened, so every later attempt took this same branch again and the
  // four-teardown ladder was unreachable for the life of the process - the one apparatus built to
  // catch 0x80bc0008 coming back could never run on the only firmware where it would matter.
  if(g_opened==0) {
    const int32_t rc = rawInit();
    ps4_log("ime: panel 1 -> sceImeDialogInit 0x%08x%s",uint32_t(rc),
            rc==0 ? "" : " - falling through to the teardown ladder");
    if(rc==0) {
      ++g_opened;
      g_up = true;
      return true;
      }
    }

  // Every later panel - and a refused first one - walks the ladder. Each attempt's Init code is logged, so a run says exactly
  // which teardown the framework accepted - or that none of them did, which is equally a result.
  for(uint32_t attempt=0; attempt<4; ++attempt) {
    const char*   what = tryTeardown(attempt);
    const int32_t rc   = rawInit();
    ps4_log("ime: panel %u attempt %u after %s -> sceImeDialogInit 0x%08x%s",
            g_opened+1,attempt,what,uint32_t(rc),
            rc==0 ? "  <- THIS teardown is the one that works" : "");
    if(rc==0) {
      ++g_opened;
      g_up = true;
      return true;
      }
    }

  ps4_log("ime: panel %u could not be raised by any of the four teardowns. A plain retry HAS "
          "worked on this console when the previous panel closed seconds earlier, so this is a "
          "state or timing problem rather than a per-process limit. The save is unaffected; the "
          "name stays as it was.",g_opened+1);
  return false;
  }

ImeState imePoll(std::string& out) {
  if(!g_up)
    return ImeState::Idle;

  const OrbisDialogStatus st = sceImeDialogGetStatus();
  if(st==ORBIS_DIALOG_STATUS_RUNNING) {
    g_noneRuns = 0;
    return ImeState::Running;
    }
  if(st!=ORBIS_DIALOG_STATUS_STOPPED) {
    // ⚠ NONE IS BOTH "NOT YET" AND "NOT ANY MORE", AND ONLY ONE OF THOSE IS WORTH WAITING FOR.
    //
    // The enum has three values: NONE=0, RUNNING=1, STOPPED=2. NONE is what you get before the
    // framework has answered - which is why this arm keeps waiting - but it is ALSO what you get
    // after it has let go, and nothing distinguishes them. This used to return Running for NONE
    // with no bound at all.
    //
    // That is a hang with no way out, not a slow path: SavNameDialog swallows EVERY key and mouse
    // event while imeUp, K_ESCAPE included, precisely so a pad press cannot reach the dialog behind
    // the panel. So a panel that quietly went away left the save dialog unclosable and the game
    // stuck until the console was rebooted.
    //
    // ~6 s at 60 Hz. Long enough that a slow framework is not cut off, short enough that a player
    // is not left staring at a dialog that will never answer. Giving up reports Cancelled, which is
    // the outcome the dialog already knows how to handle: the name stays as it was.
    if(++g_noneRuns < kNoneGiveUp)
      return ImeState::Running;
    ps4_log("ime: panel %u reported NONE for %u polls - the framework let go without a result. "
            "Giving up so the dialog can close; the savegame name is unchanged.",
            g_opened,g_noneRuns);
    sceImeDialogTerm();
    g_up       = false;
    g_noneRuns = 0;
    return ImeState::Cancelled;
    }
  g_noneRuns = 0;

  OrbisDialogResult res = {};
  const int32_t     rc  = sceImeDialogGetResult(&res);
  const bool        ok  = (rc==0 && res.endstatus==ORBIS_DIALOG_OK);
  if(ok)
    out = toUtf8(g_text);
  ps4_log("ime: panel %u result 0x%08x endstatus=%d -> %s%s%s",
          g_opened,uint32_t(rc),int(res.endstatus),
          ok ? "accepted \"" : "cancelled",ok ? out.c_str() : "",ok ? "\"" : "");

  const int32_t term = sceImeDialogTerm();
  if(term!=0)
    ps4_log("ime: sceImeDialogTerm -> 0x%08x",uint32_t(term));
  g_up = false;
  return ok ? ImeState::Accepted : ImeState::Cancelled;
  }

void imeEnd() {
  if(!g_up)
    return;
  sceImeDialogForceClose();
  sceImeDialogTerm();
  g_up = false;
  }

}

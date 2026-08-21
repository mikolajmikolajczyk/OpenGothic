#pragma once

// The system on-screen keyboard, for naming a savegame.
//
// ------------------------------------------------------------------ what it is for
//
// OpenGothic's save menu names a slot through `SavNameDialog` (game/ui/gamemenu.cpp:801): a modal
// that collects characters from `keyUpEvent` one at a time. The PS4 has no keyboard, so nothing
// arrives, the name stays empty and the field renders as the bare cursor `_`. The save itself
// works - measured on the console - but every slot is nameless.
//
// So this is not a new feature bolted on: it is the missing input device for a dialog OpenGothic
// already has. `SavNameDialog` is left intact and still works if a USB keyboard is attached; on
// PS4 it also raises this panel and takes the result.
//
// --------------------------------------------------- what the standalone ime probe measured
//
// Five console runs of a standalone ime probe settled the parts that would otherwise be
// guessed here, and two of them shape this interface:
//
//   * THE PANEL COMPOSITES OVER OUR OWN VideoOut FLIP, and the demo always kept flipping while it
//     was up. So this is polled from a place that runs every frame rather than from a blocking
//     loop of its own: whether the panel stays visible when the title stops flipping is untested,
//     and a save dialog is not where to find out.
//   * PLACEMENT IS DEVICE PIXELS, and `0,0` with LEFT/TOP works too. This uses LEFT/TOP because it
//     depends on no coordinate convention and therefore cannot be broken by a firmware that
//     changes one.
//   * strings are UTF-16 while `wchar_t` is 32-bit for this triple, so everything crossing the ABI
//     is char16_t and cast once at the boundary.
//   * `sceCommonDialogIsUsed()` reads 0 for a dialog that is plainly running. Not used.
//
// ------------------------------------------------------------------ and the claim that was wrong
//
// I recorded "ONE DIALOG PER PROCESS LAUNCH" as a hardware fact: the probe's second
// `sceImeDialogInit` was refused with 0x80bc0008 whatever its parameters, twice, and
// `sceImeDialogTerm` returning 0 did not change it. `imeBegin` was built around it - a ladder of
// four teardown sequences, and the demo's rung selection made a build knob rather than a loop
// because of it.
//
// IN THE GAME, THE SECOND PANEL OPENED ON A PLAIN RETRY, first attempt, 0x0. Both saves were named
// ("test", then "test5").
//
// The difference is TIME. The probe called Init again in the same frame as Term - immediately - and
// the game's second save came 41 seconds after the first. So 0x80bc0008 was Init arriving before
// the framework had finished releasing the previous dialog, not a per-process limit.
//
//   LEDGER: "IT ALWAYS FAILS" MEASURED TWICE IN THE SAME MILLISECOND IS ONE MEASUREMENT, NOT TWO.
//   Both demo runs re-opened at the same instant relative to Term, so they could not distinguish a
//   per-process limit from a settling time - and I generalised from the harder of the two readings
//   and designed around it.
//
// The ladder stays, and rung 0 - the plain retry - is what proved this. It costs one extra Init on
// a path that runs when a player names a save, it names whichever sequence works, and it is the
// thing that would catch the restriction coming back.
#include <cstdint>
#include <string>

namespace Ps4Og {

enum class ImeState : uint8_t {
  Idle = 0,      // nothing raised
  Running,       // the panel is up
  Accepted,      // the user confirmed; `text` is filled
  Cancelled,     // the user backed out
  Unavailable,   // the panel could not be raised at all, and the log says why
  };

// Loads the two sysmodules and initialises the common-dialog framework. Safe to call repeatedly;
// the work happens once. Returns false if any step refused, in which case imeBegin will too.
bool imeInit();

// Raise the panel with `initial` as its starting text. False means it could not be raised - every
// attempt's return code is on the log, including the teardown ladder's.
bool imeBegin(const std::string& initial);

// Call once per frame while the state is Running. On Accepted, `out` holds the text as UTF-8.
ImeState imePoll(std::string& out);

// Close and release. Idempotent.
void imeEnd();

}

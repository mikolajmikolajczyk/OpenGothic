// PS4 boot glue for OpenGothic: where the game data is, and what argv the title gets.
//
// A .pkg launch has no argv and no working directory a title may trust (a title's cwd
// is not /app0 and the sandbox refuses chdir, measured). Every
// answer OpenGothic normally takes from the command line therefore has to come from
// somewhere else, and this header is that somewhere.
//
// ------------------------------------------------------- the licence constraint first
//
// GAME DATA IS NEVER PACKAGED. Gothic II is not redistributable and the .pkg carries
// none of it: scripts/ps4/make-pkg.sh ships only what it is told to, and nothing here
// asks it to ship data. The title FINDS an installation that the console's owner put
// on the console's own storage, at run time, and says out loud which one it picked.
// Refusing is a first-class outcome, not an error path: a console with no Gothic II
// on it must print every path it looked at and stop, not crash.
//
// ------------------------------------------------------------------ the search
//
// 1. /app0/opengothic.cfg, key `gothic-root`. An explicit answer always wins and is
//    never second-guessed - if it is wrong the title refuses naming it, rather than
//    silently falling through to a different installation.
// 2. Otherwise every <base>/<name> in kOgDataBases x kOgDataNames, in order:
//      bases  /data, /mnt/usb0 .. /mnt/usb7, /app0
//      names  (the base itself), gothic2, Gothic2, GOTHIC2, GothicII, "Gothic II",
//             gothic, OpenGothic
//    A candidate is accepted when BOTH `Data` and `_work` resolve under it (either
//    exact or in one of the case variants a FAT/exFAT USB stick may present). That is
//    the cheap half of CommandLine::validateGothicPath(); the expensive half - that
//    _work/Data/Scripts/_compiled exists - is left to CommandLine itself, which does
//    it with a case-insensitive directory scan we would only be duplicating.
//
// /app0 is LAST and it is a development convenience, not a shipping layout: under
// an emulator the executable's directory (and ./game_data/app0) is union-mounted
// onto /app0, which is the only way to put a 3 GB installation in front of an
// emulated title without a symlink the emulator's mount containment refuses. On a
// console /app0 IS the package, so a hit there would mean somebody packaged the data
// - the log line says so in those words.
//
// ------------------------------------------------------------- /app0/opengothic.cfg
//
// Optional, plain text, `key=value` per line, `#` comments. Deliberately a SECOND file
// beside /app0/ps4-run.cfg rather than more keys in it: ps4-run.cfg is the LAUNCHER's
// (orbis-compat's include/ps4_app.h) and describes how a RUN behaves; this one describes
// what the TITLE is pointed at. Neither ships in a .pkg.
//
// ⚠ NOTHING WRITES ps4-run.cfg ANY MORE. It was a test harness, for the emulator leg
// of a harness that no longer exists; the file is read on every boot and is absent on every
// one, which is the console default. Left in place because a host that drops it still works.
//
//   gothic-root=/mnt/usb0/gothic2   explicit data root; skips the search
//   arg=-window                     one extra OpenGothic argv token, repeatable
//   log=0                           do not tee Tempest::Log onto the PS4 log channels
//   probes=1                        run the full boot probe suite. OFF by default: the four heavy
//                                   probes emit 40-60 klog lines at 8-15 ms each, and probeSavePaths
//                                   also creates /data/OpenGothic and writes five files. Only
//                                   probeVdfRead runs unconditionally, as regression armour for the
//                                   stat interposition.
//
// Same principle as ps4_app.h: nothing here asks what hardware it is running on. It
// reads a file a launcher chose to leave, and a console launch that left the same
// file would behave identically.
#pragma once

#include <string>
#include <vector>

namespace Ps4Og {

struct Boot {
  // False means: no usable installation was found. Everything looked at has already
  // been logged, by name. The caller must refuse, not continue.
  bool                     ok = false;
  // The accepted root, guest path, trailing '/'. Empty when !ok.
  std::string              root;
  // What to hand CommandLine{argc,argv}: argv[0], then `-g <root>`, then whatever
  // `arg=` lines the config carried. Storage for the char* array the caller builds.
  std::vector<std::string> argv;
  // Value of `log=` (default true): tee Tempest::Log onto ps4_log_frame().
  bool                     teeLog = true;
  };

// Reads the config, runs the search, logs every step. Safe to call once, after
// ps4_app_init() (it logs through it) and before anything else is constructed.
Boot boot();

// Report what readdir() says about `dir`: how many entries, and how many carried
// DT_DIR / DT_REG / DT_UNKNOWN.
//
// This is not diagnostics for its own sake. Tempest::Dir::scan (Engine/utility/dir.cpp)
// classifies purely on `dirent::d_type` and never falls back to stat(), and
// OpenGothic's FileUtil::caseInsensitiveSegment - which is how EVERY path under the
// game root is resolved - only accepts an entry whose type matches. A filesystem that
// answers DT_UNKNOWN (which FreeBSD permits, and which FAT/exFAT drivers commonly do)
// would make every directory segment unresolvable, and the failure would surface far
// away as "invalid gothic path". One line here names it at the source.
void probeDirent(const std::string& dir);

// Read a real .vdf under `root` four ways - the kernel's raw stat bytes, the corrected
// fstat(), a plain read(), and a file-backed mmap - and print all of them.
//
// This probe was written to decide WHY the console run of 2026-08-02 failed with
// `VFS disk signature not recognized: ""` on a file whose bytes were verified clean.
// It did: the run of the same evening reported `mmap agrees with read()`, which cleared
// mmap, and `fstat st_size=128` for a 64857-byte file, which convicted the SDK's
// `struct stat` layout (the overlay's orbis_stat.h has the full decode of both fields).
//
// It stays in for two reasons. It is REGRESSION ARMOUR: its VERDICT line now asserts
// that the stat interposition is live and that fstat's answer equals the kernel's own
// quadword at 0x48, so a build where the interposition silently stopped resolving says
// so in one line instead of failing 800 lines later as an unreadable archive. And it is
// the only thing that CONFIRMS the 0x48 offset - that field is reconstructed rather
// than cited, and printing the raw quadwords next to the corrected answer is what turns
// it into a measurement.
//
// Cheap: one open, one raw stat, one 16-byte read, one page-sized map.
void probeVdfRead(const std::string& root);

// List what the installation actually CONTAINS: the root's entries, then every file in
// Data/ with its size, then a verdict on the two archives the user interface cannot work
// without.
//
// Console run 4 (2026-08-02 22:47) presented frames but painted nothing, because every
// font failed: `failed to open resource: font_old_20_white.fnt` x4. No archive reported
// an error, which reads as "all archives mounted" but only means "every archive that was
// FOUND mounted" - Resources::detectVdf enumerates the directory and mounts what is
// there, so a missing archive is silent by construction.
//
// The evidence was already in this file's own readdir probe and was easy to miss:
// `/data/gothic2/` reported 5 entries, DT_DIR 5, DT_REG 0 - i.e. '.', '..' and three
// directories, no files. The same probe against a full install reports 12 entries,
// 9 directories and 3 files. The console copy is a partial upload.
//
// Measured against a stock Gothic II NotR install: every .FNT lives in exactly two
// archives, Textures.vdf (305 MB) and Textures_Addon.vdf (147 MB), and in no other -
// checked across all 15. So a data root without Textures.vdf mounts cleanly, loads
// scripts, opens a window, presents frames, and draws nothing at all. That failure mode
// is expensive to diagnose from a black screen and trivial to diagnose from a file list.
void probeDataInventory(const std::string& root);

// Case folding, i.e. whether ZenKit's case-insensitive VFS lookup can work at all.
// See the implementation for why this one question earns a probe of its own.
// probeCtype() MOVED to orbis-compat: orbis::probeCtype(), <orbis_boot.h>

// probeLargestArchiveMap() REMOVED. It mapped the biggest archive whole and touched three bytes,
// and it proved its point before becoming a hazard: 37 s on the console and 689 MiB of I/O on every
// boot, which is the measurement that established PS4 mmap POPULATES EAGERLY. Its call site had been
// commented out for a while and the function was never deleted, so this header went on promising a
// probe that no longer ran. The measurement it earned is kept at the foot of probeVdfRead().

// Where a savegame can go, and whether a relative path reaches it.
//
// OpenGothic writes savegames through `Tempest::WFile f("save_slot_0.sav")` - a BARE
// RELATIVE NAME, built in six places (game/ui/gamemenu.cpp x4, gothic.cpp x2) and read back
// by `FileUtil::exists`, which is a `stat` of the same relative name. So every save and load
// in the game resolves against the process's working directory.
//
// AND THIS SANDBOX REFUSES chdir. Measured on the console twice:
// `chdir("/app0")=-1`, and the SDK exports no sceKernelChdir to fall back on. That suite
// solved its own version of this by interposing `open` at LINK time - a strong definition
// beats libc.a's archive member - and rewriting a leading relative component.
//
// Before that mechanism is repeated for saves, three things have to be MEASURED rather than
// assumed, because each one changes what the fix is:
//
//   * what the working directory actually IS. If it is already writable, relative saves work
//     today and there is nothing to interpose.
//   * whether mkdir works at all here. /data is writable under GoldHEN; nothing has tested
//     whether a title may create a directory in it.
//   * which root survives a real create-write-stat-read-unlink round trip. A path that
//     accepts open(O_CREAT) and then fails on the write is the worst of the three outcomes
//     and the one a naive probe would report as success.
//
// Every step logs its own errno, and the last line names the root a save should use. It runs
// at boot rather than at the first save: a save is a player action deep in a session and this
// answer is wanted before that.
void probeSavePaths(const std::string& root);

// Replace CrashLog's terminate/signal handlers with ones that SAY SOMETHING.
//
// OpenGothic's CrashLog writes its dump to std::cout and a crash.log next to the
// executable, then std::abort()s. On a console std::cout goes nowhere, /app0 is not
// writable, and abort() from a homebrew title is exactly the CE-34878-0 error dialog
// with no further information: the single most common outcome of this port right now
// would be a crash with no evidence at all. These handlers print the exception's type
// and what() over netlog/klog, then hold the process with ps4_idle_forever() so the
// log actually leaves the machine.
//
// Call AFTER CrashLog::setup(), which is what they are overriding.
// installCrashHandlers() MOVED to orbis-compat: orbis::installCrashHandlers(), <orbis_boot.h>

}

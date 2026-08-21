#include <Tempest/Window>
#include <Tempest/Application>
#include <Tempest/Log>

#include <zenkit/Logger.hh>

#include <Tempest/VulkanApi>

#if defined(__PS4__)
// orbis-compat carries the crash handlers now - it is what supplies backtrace(3) for them to grow
// into, and neither they nor the ctype probe were ever about Gothic. See <orbis_boot.h>.
#include <orbis_boot.h>
// This console has no Vulkan loader; orbis-mesa's RADV is linked in through orbis-compat's
// vkloader, which supplies the C ABI in its place. ACO compiles the SPIR-V at run time, so
// there is no shader bake on this path.
#include <vector>
#include <string>
#include <cstdlib>   // setenv, for the driver-log environment below
#include <cstdio>    // fopen/fgets, for /data/tempest-env.txt
#include <cstring>   // strchr/strpbrk, same
#include "ps4_app.h"
#include "og_ps4_boot.h"
#define PS4_STAGE(x) ps4_log("stage: " x)
#else
#define PS4_STAGE(x) do {} while(false)
#endif

#if defined(_MSC_VER)
#include <Tempest/DirectX12Api>
#endif

#if defined(__APPLE__)
#include <Tempest/MetalApi>
#endif

#if defined(__IOS__)
#include "utils/installdetect.h"
#endif

#include "utils/crashlog.h"
#include "mainwindow.h"
#include "gothic.h"
#include "build.h"
#include "commandline.h"

#include <dmusic.h>

std::string_view selectDevice(const Tempest::AbstractGraphicsApi& api) {
  auto d = api.devices();

  static Tempest::Device::Props p;
  for(auto& i:d)
    // if(i.type==Tempest::DeviceType::Integrated) {
    if(i.type==Tempest::DeviceType::Discrete) {
      p = i;
      return p.name;
      }
  if(d.size()>0) {
    p = d[0];
    return p.name;
    }
  return "";
  }

std::unique_ptr<Tempest::AbstractGraphicsApi> mkApi(const CommandLine& g) {
  Tempest::ApiFlags flg = g.isValidationMode() ? Tempest::ApiFlags::Validation : Tempest::ApiFlags::NoFlags;
#if defined(__PS4__)
  // One backend, no choice to make: -dx12 is unreachable here and RADV is the only Vulkan.
  return std::make_unique<Tempest::VulkanApi>(flg);
#else
  switch(g.graphicsApi()) {
    case CommandLine::DirectX12:
#if defined(_MSC_VER)
      return std::make_unique<Tempest::DirectX12Api>(flg);
#else
      break;
#endif
    case CommandLine::Vulkan:
#if !defined(__APPLE__)
      return std::make_unique<Tempest::VulkanApi>(flg);
#else
      break;
#endif
    }

#if defined(__APPLE__)
  return std::make_unique<Tempest::MetalApi>(flg);
#else
  return std::make_unique<Tempest::VulkanApi>(flg);
#endif
#endif
  }

int main(int argc,const char** argv) {
#if defined(__IOS__)
  {
    auto appdir = InstallDetect::applicationSupportDirectory();
    std::filesystem::current_path(appdir);
  }
#endif

#if defined(__PS4__)
  // FIRST statement of main(): ps4_app_init() brings up the UDP log and klog and reads
  // /app0/ps4-run.cfg. Everything after this line - including a refusal to start - is
  // observable off the console; anything before it is not.
  ps4_app_init("opengothic",PS4_APP_STAMP);
  // ⚠ NO DRIVER KNOB IS FORCED HERE, AND THAT IS A DELIBERATE INVERSION.
  //
  // This block used to setenv() ORBIS_DRM_TRACE, RADV_DEBUG, ORBIS_TRACE_SUBMITS, ORBIS_DUMP_SUBMIT,
  // ORBIS_TRUNCATE and ORBIS_NO_PREFETCH with overwrite=1, each with the reasoning of the run it was added
  // for. The consequence was that /data/tempest-env.txt existed largely to switch those back OFF - a title
  // that turns diagnostics on and a configuration file that turns them off is backwards.
  //
  // The knobs are all still there in the driver. This file is how any of them is turned ON.
  //
  // Why a file at all: changing ONE string used to mean a rebuild, a repackage and a reflash, and the build
  // script re-clones on any patch change - so a one-character experiment cost the same as a real one. A file of
  // KEY=VALUE lines on /data costs an FTP put instead, into a directory the maintainer already has open to fetch
  // the logs. Same trip, next experiment, no rebuild:
  //
  //     # /data/tempest-env.txt
  //     ORBIS_NO_PREFETCH=0
  //     ORBIS_TRUNCATE=7:314
  //
  // OVERWRITE=1 ON PURPOSE: the file is the operator's word. Every applied line is echoed to the title's log, and so is the file's ABSENCE - a knob that
  // can silently not fire has to say that it did not, which is a rule this hunt paid for once already.
  {
    FILE* f = ::fopen("/data/tempest-env.txt","r");
    if(f==nullptr) {
      ps4_log("env: no /data/tempest-env.txt - the knobs compiled into this build stand");
      } else {
      char line[512];
      int  applied = 0;
      while(::fgets(line,sizeof(line),f)!=nullptr) {
        char* nl = ::strpbrk(line,"\r\n");
        if(nl!=nullptr)
          *nl = '\0';
        char* key = line;
        while(*key==' ' || *key=='\t')
          ++key;
        if(*key=='\0' || *key=='#')
          continue;
        char* eq = ::strchr(key,'=');
        if(eq==nullptr) {
          ps4_log("env: ignoring \"%s\" - no '='",key);
          continue;
          }
        // Trim inwards from the '=' on both sides, so `KEY = value` and a trailing space behave. A stray space in
        // a value like "7:314 " would otherwise reach the driver's parser and be read as a different experiment.
        char* keyEnd = eq;
        while(keyEnd>key && (keyEnd[-1]==' ' || keyEnd[-1]=='\t'))
          --keyEnd;
        *keyEnd = '\0';
        char* val = eq+1;
        while(*val==' ' || *val=='\t')
          ++val;
        char* valEnd = val + ::strlen(val);
        while(valEnd>val && (valEnd[-1]==' ' || valEnd[-1]=='\t'))
          --valEnd;
        *valEnd = '\0';
        if(*key=='\0')
          continue;
        ::setenv(key,val,1);
        ps4_log("env: %s=%s (from /data/tempest-env.txt)",key,val);
        ++applied;
        }
      ::fclose(f);
      ps4_log("env: %d knob(s) applied from /data/tempest-env.txt",applied);
      }
  }
  // A .pkg launch has no argv, and there is no working directory a title may trust, so
  // the game-data root is DISCOVERED (ps4/og_ps4_boot.h) and handed to CommandLine as a
  // synthesised `-g <root>`. No game data ships in the package.
  const Ps4Og::Boot ps4Boot = Ps4Og::boot();
  std::vector<const char*> ps4Argv;
  if(ps4Boot.teeLog)
    Tempest::Log::setOutputCallback([](Tempest::Log::Mode, const char* text) {
      // ps4_log_frame() and not ps4_log(): UDP always, klog only when the host asked
      // for it (`frame-klog=1`). A console klog write blocks the writer for seconds
      // when nothing drains the channel, and OpenGothic logs far too much for that.
      ps4_log_frame("%s",text);
      });
  if(!ps4Boot.ok) {
    // Already logged, at length, naming every path searched.
    ps4_idle_forever("no game data");
    return 1;
    }
  for(const auto& a:ps4Boot.argv)
    ps4Argv.push_back(a.c_str());

  // ⚠ EXTRA ARGV FROM THE ENVIRONMENT, WHICH ON THIS PATH MEANS FROM /data/tempest-env.txt AND WITHOUT A REBUILD.
  //
  // OpenGothic's own switches live in argv, and a .pkg launch has none - og_ps4_boot synthesises `-g <root>` plus
  // whatever `arg=` lines /app0/ps4-run.cfg carries. But /app0 is INSIDE the package: changing one switch there
  // costs a full rebuild, a repackage and a reflash, which is what the env file exists to avoid.
  //
  // WHY IT IS NEEDED NOW. The hung-submission dumper caught the defect on its first run: submit #6500, 184 dwords,
  // and it contains no draw at all - it is `IT_DISPATCH_DIRECT 7,1,1` with PARTIAL_TG_EN and NUM_THREAD_PARTIAL=21,
  // and the only code in Mesa that emits an unaligned dispatch is ACCELERATION-STRUCTURE BUILDING
  // (vk_acceleration_structure.c:610,674,1144,1205 through radv_unaligned_dispatch). OpenGothic is building
  // ray-tracing BVHs while the world loads, and one of those builds hangs this GPU.
  //
  // `OG_ARGS=-rt 0` in the env file turns ray queries off at the source - gothic.cpp:88 only sets doRayQuery when
  // the device advertises rayQuery AND CommandLine says so - so no acceleration structure is built and nothing
  // dispatches unaligned. Space-separated, so several switches fit: `OG_ARGS=-rt 0 -window`.
  //
  // Kept general rather than a single -rt gate: every future OpenGothic switch is now one FTP put away, and the
  // tokens are logged so a run always states what it was given.
  static std::vector<std::string> ps4EnvArgs;
  if(const char* extra = std::getenv("OG_ARGS")) {
    std::string tok;
    for(const char* c = extra; ; ++c) {
      if(*c==' ' || *c=='\t' || *c=='\0') {
        if(!tok.empty()) {
          ps4EnvArgs.push_back(tok);
          tok.clear();
          }
        if(*c=='\0')
          break;
        continue;
        }
      tok.push_back(*c);
      }
    for(const auto& a:ps4EnvArgs)
      ps4Argv.push_back(a.c_str());
    ps4_log("argv: %d extra token(s) from OG_ARGS=\"%s\"", int(ps4EnvArgs.size()), extra);
    }

  argc = int(ps4Argv.size());
  argv = ps4Argv.data();
#else
  try {
    static Tempest::WFile logFile("log.txt");
    Tempest::Log::setOutputCallback([](Tempest::Log::Mode mode, const char* text) {
      logFile.write(text,std::strlen(text));
      logFile.write("\n",1);
      if(mode==Tempest::Log::Error)
        logFile.flush();
      });
    }
  catch(...) {
    Tempest::Log::e("unable to setup logfile - fallback to console log");
    }
#endif
  CrashLog::setup();
#if defined(__PS4__)
  // After CrashLog::setup(), which it overrides: CrashLog's handlers write to
  // std::cout and abort(), i.e. they produce CE-34878-0 and no evidence.
  orbis::installCrashHandlers();
#endif

  zenkit::Logger::set(zenkit::LogLevel::INFO, [] (zenkit::LogLevel lvl, const char* cat, const char* message) {
    (void)cat;
    switch (lvl) {
      case zenkit::LogLevel::ERROR:
        Tempest::Log::e("[zenkit] ", message);
        break;
      case zenkit::LogLevel::WARNING:
        Tempest::Log::e("[zenkit] ", message);
        break;
      case zenkit::LogLevel::INFO:
        Tempest::Log::i("[zenkit] ", message);
        break;
      case zenkit::LogLevel::DEBUG:
      case zenkit::LogLevel::TRACE:
        Tempest::Log::d("[zenkit] ", message); // unused
        break;
      }
    });
  Dm_setLogger(DmLogLevel_INFO, [](void* ctx, DmLogLevel lvl, char const* msg) {
    switch (lvl) {
      case DmLogLevel_FATAL:
      case DmLogLevel_ERROR:
      case DmLogLevel_WARN:
        Tempest::Log::e("[dmusic] ", msg);
        break;
      case DmLogLevel_INFO:
        Tempest::Log::i("[dmusic] ", msg);
        break;
      case DmLogLevel_DEBUG:
      case DmLogLevel_TRACE:
        Tempest::Log::d("[dmusic] ", msg);
        break;
      }
    }, nullptr);

  Tempest::Log::i(appBuild);
  Workers::setThreadName("Main thread");

  // The startup sequence, announced stage by stage. On a console a title that stops
  // gives no other clue where it stopped: there is no debugger attached, no stdout and
  // no core file, and the only two observable outcomes are "the log stops here" and
  // the CE-34878-0 dialog. Naming each stage BEFORE it runs turns both into a
  // location. Nothing outside __PS4__ changes.
  PS4_STAGE("commandline");
  CommandLine          cmd{argc,argv};
  PS4_STAGE("mkApi");
  auto                 api     = mkApi(cmd);
  PS4_STAGE("selectDevice");
  const auto           gpuName = selectDevice(*api);
  CrashLog::setGpu(gpuName);

  PS4_STAGE("device");
  Tempest::Device      device{*api,gpuName};
  CrashLog::setGpu(device.properties().name);

  PS4_STAGE("resources");
  Resources            resources{device};
  PS4_STAGE("gothic");
  Gothic               gothic;
  PS4_STAGE("gamemusic");
  GameMusic            music;
  PS4_STAGE("globalscripts");
  gothic.setupGlobalScripts();

  PS4_STAGE("mainwindow");
  MainWindow           wx(device);
  PS4_STAGE("application");
  Tempest::Application app;
  PS4_STAGE("exec");
  const int rc = app.exec();
  PS4_STAGE("exec returned");
#if defined(__PS4__)
  // Returning from main() on a retail console tears the process down outside the
  // system's expected path and pops CE-34878-0 (orbis-compat's include/ps4_app.h). This returns
  // only when the host left `autoexit=1` in /app0/ps4-run.cfg, which no .pkg does.
  ps4_idle_forever("exec returned");
#endif
  return rc;
  }

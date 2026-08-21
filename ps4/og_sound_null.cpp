// A silent Tempest sound backend, linked into the PS4 build of OpenGothic only.
//
// ------------------------------------------------------------------ why this exists
//
// Engine/CMakeLists.txt forces TEMPEST_BUILD_AUDIO=OFF on PS4 (openal-soft has no
// Orbis backend), and all three sound TUs — Engine/sound/{sounddevice,sound,
// soundeffect}.cpp — are wholly inside `#if defined(TEMPEST_BUILD_AUDIO)`. The
// HEADERS are not guarded, so every caller COMPILES and the whole set fails at LINK:
// libTempest.a built for PS4 defines zero sound symbols. OpenGothic has no -nosound
// switch and constructs three SoundDevice objects before Application::exec()
// (Resources::sound, Gothic::sndDev, GameMusic::device), derives from SoundProducer
// twice, and plays menu music before the first frame — so "just don't call it" is not
// available. Measured 2026-08-02: 20 undefined symbols at the error limit, and the
// full set is what this file defines.
//
// ------------------------------------------------------------------ why HERE and not
//                                                                     in Engine/sound
//
// The eventual home of a null backend is Engine/sound itself, behind the same
// TEMPEST_BUILD_AUDIO switch, so that ANY title cross-built for PS4 links. That is a
// change to an upstream-owned file for the sake of one downstream title, and it would
// have to be designed together with the REAL backend (sceAudioOut), which is a
// separate task. Until then this TU sits in the PS4 side of the tree, is added to the
// OpenGothic target by this title's own CMakeLists.txt alone, and touches
// nothing upstream owns. Nothing else in the repo compiles it.
//
// ------------------------------------------------------------------ what it does
//
// Nothing, audibly, and it says so once. Every object is constructible, movable and
// destructible; every query returns the value that reads as "this sound is not
// playing and never will": isEmpty()==true, isFinished()==true, timeLength()==0.
//
// isFinished()==true is the load-bearing one. WorldSound and DialogMenu poll it to
// retire a slot; a null effect that claimed to still be running would leak a slot per
// sound for the life of the process. Returning 0 for timeLength() is the matching
// choice for aiOutput()'s dialogue pacing: the dialogue advances immediately instead
// of waiting on speech that will never play. Both are deliberate — a silent title
// that stalls on a missing voice line is worse than a silent title that reads its
// subtitles fast.
//
// What it does NOT do is stand still. See "the clock" below: renderSound() is the
// only thing that moves a title's audio clock, and a device that never calls it is
// not a silent device, it is a stopped one.
// The public spelling. These were `../../Engine/sound/*.h`, a relative path left over from when
// this file lived under lib/Tempest/ - see og_sound_orbis.cpp for what that resolved through.
#include <Tempest/SoundDevice>
#include <Tempest/Sound>
#include <Tempest/SoundEffect>

#include "ps4_app.h"

#include <orbis/libkernel.h>

#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// THE CONTROL RUNG. This file is one of two sound backends the PS4 target compiles, and
// the real one - og_sound_orbis.cpp, a mixer on sceAudioOut - is the default. Each guards its whole
// body on this knob so that exactly one defines the Tempest sound symbols and no CMake branch has to
// be kept in step with it.
//
// It is not a fallback to reach for when something sounds wrong. It is the build that separates "the
// defect is in the audio path" from "the defect is beside it": everything above still holds, so a run
// at OG_SOUND_NULL=1 is a silent title that is otherwise identical.
#if defined(OG_SOUND_NULL)

using namespace Tempest;

// One line, the first time anything asks for audio. Announced rather than silent:
// "the title made no sound" and "the title's audio backend is a stub" are different
// facts and the log must not conflate them.
static void announceOnce() {
  static bool said = false;
  if(said)
    return;
  said = true;
  ps4_log("sound: null backend - the title runs silent, the audio clock advances at nominal rate");
  }

// ------------------------------------------------------------------ the clock
//
// A device that produces no sound still has to CONSUME its sources, because
// SoundProducer::renderSound() is the only thing that advances a title's audio
// clock and several things read that clock rather than the wall clock.
// OpenGothic's video widget is one: game/ui/videowidget.cpp:126-141 paces every
// cutscene frame on min(wall clock, audio clock) and sleeps the difference, so an
// audio clock pinned at zero makes frame N sleep 1000*den*N/num ms - the whole
// video costs time quadratic in its length. Measured on console 2026-08-04
// (build-ps4-logs/ps4-udp-20260804-193503.log): INTRO.BIK reached frame 25 of 2845
// in 93.1 s and Addon_Title.BIK frame 24 of 234 in 85.2 s, both a factor ~7 above
// even that quadratic - the residual belongs to Application::sleep(), not here.
//
// So: one thread, every playing producer pumped at its own nominal sample rate,
// output discarded. The time source is sceKernelGetProcessTime() - microseconds,
// monotonic, no wall-clock jumps, and the same source every ps4/ demo times with.
// Drift has nothing to drift against: no samples reach a DAC, so "nominal rate" is
// the whole contract, and a consumer that is slaved to this clock (the video widget
// is) simply runs at the rate the pump keeps.
namespace {

struct NullMixer {
  enum {
    // A quarter of the audio a 25 fps Bink frame pushes, so the pump can never ask
    // for more than is buffered under normal pacing - see the starvation note in
    // run(). At 44100 Hz stereo that is 441 frames against the 1764 a video frame
    // supplies.
    BlockUs        = 10000,
    MaxBlockFrames = 512,
    MaxCatchUp     = 4,
    };

  struct Voice {
    Tempest::SoundProducer* producer  = nullptr;
    uint32_t                frequency = 44100;
    bool                    active    = false;
    };

  void add(Tempest::SoundProducer* p, uint32_t frequency) {
    std::lock_guard<std::mutex> guard(sync);
    Voice v;
    v.producer  = p;
    v.frequency = frequency;
    voice.push_back(v);
    if(!started) {
      started = true;
      std::thread(&NullMixer::run,this).detach();
      }
    }

  void remove(Tempest::SoundProducer* p) {
    std::lock_guard<std::mutex> guard(sync);
    for(size_t i=0; i<voice.size(); ++i)
      if(voice[i].producer==p) {
        voice.erase(voice.begin()+ptrdiff_t(i));
        return;
        }
    }

  void setActive(Tempest::SoundProducer* p, bool a) {
    std::lock_guard<std::mutex> guard(sync);
    for(auto& v:voice)
      if(v.producer==p)
        v.active = a;
    }

  void run() {
    // Four int16 per frame, not the two a stereo producer owes for renderSound(out,n).
    // OpenGothic's music providers write `n * sizeof(int32_t) * 2` bytes whenever the
    // music is disabled (game/gamemusic.cpp:108 and :242), which is twice the buffer
    // the interface promises them. That over-write is upstream's and the real openal
    // backend takes it too; a null device is not the place to rediscover it as a
    // corrupted heap on a console.
    int16_t  buf[MaxBlockFrames*4] = {};
    uint64_t last = sceKernelGetProcessTime();

    for(;;) {
      sceKernelUsleep(BlockUs);

      const uint64_t now = sceKernelGetProcessTime();
      size_t         n   = size_t((now-last)/BlockUs);
      if(n>MaxCatchUp) {
        // A pump that fell behind must not ask for a bigger block to catch up: a
        // producer with less than the whole request buffered returns having consumed
        // NOTHING (videowidget.cpp:91), so the debt would starve the clock instead of
        // repaying it. Drop it and keep the rate.
        n    = MaxCatchUp;
        last = now;
        } else {
        last += uint64_t(n)*BlockUs;
        }

      // The lock is held across renderSound() so a producer cannot be destroyed under
      // the pump. That is safe only because nothing reachable from renderSound() ever
      // registers, unregisters, plays or pauses an effect - checked against both
      // OpenGothic producers: the video widget takes its own samples mutex and the
      // music provider takes its own pending mutex, and neither owns a SoundEffect.
      std::lock_guard<std::mutex> guard(sync);
      for(auto& v:voice) {
        if(!v.active)
          continue;
        size_t frames = size_t(v.frequency)*BlockUs/1000000u;
        if(frames>MaxBlockFrames)
          frames = MaxBlockFrames;
        for(size_t i=0; i<n; ++i)
          v.producer->renderSound(buf,frames);
        }
      }
    }

  std::mutex         sync;
  std::vector<Voice> voice;
  bool               started = false;
  };

NullMixer& nullMixer() {
  // Never destroyed. The pump thread is detached and a title on this console ends by
  // having its process torn down, so a static destructor racing a running mixer is
  // the one failure nobody could read out of a UDP log.
  static NullMixer* m = new NullMixer();
  return *m;
  }

}

// ------------------------------------------------------------------ SoundDevice

struct SoundDevice::Data {
  float globalVolume = 1.f;
  };

SoundDevice::SoundDevice() : data(new Data()) {
  announceOnce();
  }

SoundDevice::SoundDevice(std::string_view /*name*/) : data(new Data()) {
  announceOnce();
  }

SoundDevice::~SoundDevice() = default;

std::vector<SoundDevice::Props> SoundDevice::devices() {
  // An empty list is the honest answer and the one every caller already handles
  // (OpenGothic only enumerates devices for its settings menu).
  return std::vector<Props>();
  }

SoundEffect SoundDevice::load(const char* /*fname*/) {
  announceOnce();
  return SoundEffect();
  }

SoundEffect SoundDevice::load(Tempest::IDevice& /*d*/) {
  announceOnce();
  return SoundEffect();
  }

SoundEffect SoundDevice::load(const Sound& /*snd*/) {
  announceOnce();
  return SoundEffect();
  }

SoundEffect SoundDevice::load(std::unique_ptr<SoundProducer>&& p) {
  announceOnce();
  // THE PRODUCER MUST STAY ALIVE. This is an ownership contract, not an optimisation:
  // `load` takes the unique_ptr, and the returned SoundEffect owns the producer for its
  // whole lifetime. Callers keep a RAW pointer to it and go on using it afterwards -
  // OpenGothic's GameMusic is exactly that shape (gamemusic.h:54 `MusicProvider* impl`,
  // set from p.get() at gamemusic.cpp:404, still called at :359/:363/:380 long after
  // `sound = device.load(std::move(p))` at :407).
  //
  // An earlier version of this file dropped it here, reasoning that a producer nothing
  // will ever pump is a leak. That reasoning was wrong and it cost a console run: the
  // provider's `std::recursive_mutex pendingSync` was destroyed inside load(), and
  // GameMusic::setEnabled() - four statements later - locked it. On hardware Sony's
  // pthread_mutex_lock refuses a destroyed mutex with EINVAL and libc++ turns that into
  // `std::system_error(recursive_mutex lock failed: Invalid argument)`, which is where
  // console run 3 (2026-08-02 22:35:32) died, 8 ms after "Switching music provider".
  // Under an emulator the same lock returned success and the bug was invisible.
  return SoundEffect(*this,std::move(p));
  }

void SoundDevice::process() {
  }

void SoundDevice::suspend() {
  }

void SoundDevice::setListenerPosition(const Tempest::Vec3&) {
  }

void SoundDevice::setListenerPosition(float, float, float) {
  }

void SoundDevice::setListenerDirection(const Tempest::Vec3&, const Tempest::Vec3&) {
  }

void SoundDevice::setListenerDirection(float, float, float, float, float, float) {
  }

void SoundDevice::setGlobalVolume(float v) {
  if(data!=nullptr)
    data->globalVolume = v;
  }

void* SoundDevice::context() {
  return nullptr;
  }

// ------------------------------------------------------------------ Sound

Sound::Data::~Data() = default;

uint64_t Sound::Data::timeLength() const {
  return 0;
  }

Sound::Sound(const char*) {
  }

Sound::Sound(const std::string&) {
  }

Sound::Sound(const char16_t*) {
  }

Sound::Sound(const std::u16string&) {
  }

Sound::Sound(IDevice&) {
  }

bool Sound::isEmpty() const {
  return true;
  }

uint64_t Sound::timeLength() const {
  return 0;
  }

// ------------------------------------------------------------------ SoundEffect

SoundProducer::SoundProducer(uint16_t frequency, uint16_t channels)
  : frequency(frequency), channels(channels) {
  announceOnce();
  }

struct SoundEffect::Impl {
  ~Impl() {
    if(producer!=nullptr)
      nullMixer().remove(producer.get());
    }

  float volume = 1.f;
  Vec3  pos;
  // Owned. See SoundDevice::load(unique_ptr<SoundProducer>&&): the caller may keep a
  // raw pointer to this object and use it for as long as the SoundEffect lives, so
  // its lifetime is the effect's, not load()'s. The mixer holds a borrowed pointer
  // and gives it back in ~Impl, above, while the producer is still alive.
  std::unique_ptr<SoundProducer> producer;
  };

SoundEffect::SoundEffect() = default;

SoundEffect::SoundEffect(SoundEffect&& s) : impl(std::move(s.impl)) {
  }

SoundEffect::~SoundEffect() = default;

SoundEffect& SoundEffect::operator=(SoundEffect&& s) {
  impl = std::move(s.impl);
  return *this;
  }

SoundEffect::SoundEffect(SoundDevice&, const Sound&) {
  }

SoundEffect::SoundEffect(SoundDevice&, std::unique_ptr<SoundProducer>&& src)
  : impl(new Impl()) {
  impl->producer = std::move(src);
  nullMixer().add(impl->producer.get(), impl->producer->frequency);
  }

void SoundEffect::play() {
  announceOnce();
  if(impl!=nullptr && impl->producer!=nullptr)
    nullMixer().setActive(impl->producer.get(), true);
  }

void SoundEffect::pause() {
  if(impl!=nullptr && impl->producer!=nullptr)
    nullMixer().setActive(impl->producer.get(), false);
  }

bool SoundEffect::isEmpty() const {
  return true;
  }

bool SoundEffect::isFinished() const {
  // See the header comment: a slot that never retires is a slot leaked per sound.
  return true;
  }

uint64_t SoundEffect::timeLength() const {
  return 0;
  }

uint64_t SoundEffect::currentTime() const {
  return 0;
  }

void SoundEffect::setPosition(const Tempest::Vec3& p) {
  if(impl!=nullptr)
    impl->pos = p;
  }

void SoundEffect::setPosition(float x, float y, float z) {
  setPosition(Vec3(x,y,z));
  }

void SoundEffect::setMaxDistance(float) {
  }

void SoundEffect::setVolume(float v) {
  if(impl!=nullptr)
    impl->volume = v;
  }

float SoundEffect::volume() const {
  return impl!=nullptr ? impl->volume : 0.f;
  }

Tempest::Vec3 SoundEffect::position() const {
  return impl!=nullptr ? impl->pos : Vec3();
  }

float SoundEffect::x() const {
  return position().x;
  }

float SoundEffect::y() const {
  return position().y;
  }

float SoundEffect::z() const {
  return position().z;
  }

#endif // OG_SOUND_NULL

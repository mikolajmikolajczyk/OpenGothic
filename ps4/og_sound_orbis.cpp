// A real Tempest sound backend on sceAudioOut, linked into the PS4 build of OpenGothic.
//
// ------------------------------------------------------------------ what the hardware said
//
// Every number below is from a standalone audio probe on the console, run twice identically.
// Nothing here is inferred from a header or from another platform's driver.
//
//   * THE MAIN PORT IS 48000 Hz AND NOTHING ELSE. 44100, 32000, 24000, 22050, 16000, 12000, 11025 and
//     8000 all return 0x80260008. Gothic's own assets are 22050, 44100, 11025 and (two files) 43344,
//     so EVERY voice is resampled here. That is not an optimisation deferred to later - it is on the
//     path of the first sample that plays.
//   * GRAIN IS A MULTIPLE OF 256, 256..2048. 128 and 300 return 0x80260006 - a different code from a
//     bad rate, so a refusal names which parameter was wrong.
//   * sceAudioOutOutput IS THE CLOCK. 64 calls at grain 256 measured min 5255, mean 5332, max 5425 us
//     against 5333 us of audio in a block: 0.02% off theory. So this file has NO clock, no deadline
//     scheduler and no drift correction - the hardware paces the thread. It also means the call BLOCKS,
//     which is why the mix runs on its own thread and can never be called from the render thread.
//   * ALL FOUR FORMATS OPEN, float included. This uses S16_STEREO anyway: that is the format the demo's
//     audible half actually validated by ear, and the float range is stated nowhere. A verified path
//     beats a shorter unverified one.
//   * THERE IS ONE PORT. Every Open in all three ladders returned the same handle, 0x20000007, for
//     every rate, grain and format and after every Close. That is the id of the main port, not an
//     allocation - so a process gets one output stream.
//
// ------------------------------------------------------------------ what the one port implies
//
// OpenGothic constructs THREE SoundDevice objects before Application::exec() - Resources::sound,
// Gothic::sndDev and GameMusic::device. If each opened the port they would all hold 0x20000007 and
// fight over it. So the mixer is a process-wide singleton that every SoundDevice shares, and all
// mixing happens in software before that single stream.
//
// It also means GLOBAL VOLUME CANNOT BE PROCESS-WIDE. Gothic has separate music and effect volumes and
// sets them through setGlobalVolume on DIFFERENT devices, so each device owns a gain and every voice
// remembers which device created it. A single global would make the music slider control gunshots.
//
// ------------------------------------------------------------------ what Gothic actually ships
//
// Measured by scanning the retail archives rather than assumed (Data/Sounds.vdf, Data/Speech1.vdf):
//
//   Sounds.vdf   968 files, 100% PCM 16-bit: 22050 mono (615), 44100 mono (182), 22050 stereo (73),
//                11025 mono (52), 11025 stereo (38), 44100 stereo (6), 43344 mono (2)
//   Speech1.vdf  6000 files scanned, 100% IMA-ADPCM, 4-bit, mono, 44100 Hz
//
// So 8-bit PCM never appears and is still handled (it costs four lines), OGG never appears and is
// refused by name, and ADPCM is not optional: it is every spoken line in the game.
//
// ------------------------------------------------------------------ what this is NOT, yet
//
// Distance attenuation is a linear rolloff to zero at maxDistance, not OpenAL's inverse-distance
// model, and panning is a constant-power pan from the listener's right vector. Both are stated here
// because they are audible choices a person can judge and change, not physics. There is no doppler, no
// cone, no reverb and no per-voice pitch - nothing in this interface exposes them.
// ⚠ THE PUBLIC SPELLING, and it used to be a relative path that outlived its file. These three
// were `#include "../../Engine/sound/sound*.h"` - correct when this file lived at
// lib/Tempest/ps4/opengothic/, where ../.. IS the Tempest root. From OpenGothic/ps4/ that path
// points one level ABOVE the repository and does not exist; it kept resolving only because the
// quoted-include fallback found it under -I<root>/lib/Tempest/Engine/include, which happens to
// sit exactly two levels below lib/Tempest. Tempest moving its public include directory would
// have broken all six lines with "file not found" and no hint that a file move was the cause.
//
// <Tempest/SoundDevice> and its two neighbours are one-line forwards to the same headers, so
// this is the same code through the interface Tempest actually exports.
#include <Tempest/SoundDevice>
#include <Tempest/Sound>
#include <Tempest/SoundEffect>

#include "ps4_app.h"

#include <orbis/libkernel.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

#include <Tempest/IDevice>
#include <Tempest/File>
#include <Tempest/Vec>

#include <stdexcept>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// The null backend is the control rung and lives in og_sound_null.cpp. One of the two compiles.
#if !defined(OG_SOUND_NULL)

using namespace Tempest;

namespace {

// ------------------------------------------------------------------ IMA-ADPCM
//
// Lifted from Engine/sound/sound.cpp (Sound::decodeAdPcmBlock and its two tables), which is Tempest's
// own MIT code in this fork. Copied rather than shared because the original is inside
// `#if defined(TEMPEST_BUILD_AUDIO)` - the switch that is OFF on this platform and the reason this file
// exists at all - and because reaching into an upstream TU to export one static would be a change to a
// file we have to keep mergeable.
//
// The format is WAVE_FORMAT_IMA_ADPCM (tag 17), which is what all 6000 speech files scanned are: a
// per-block preamble of (predictor, index, 0) per channel, then 4-bit nibbles, eight samples per four
// bytes per channel.
const uint16_t adpcmStep[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
  157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,
  1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
  10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
  };

const int32_t adpcmIndex[8] = { -1,-1,-1,-1,2,4,6,8 };

// Returns the number of frames written, 0 on a malformed block. `outbuf` is interleaved by channel,
// exactly as the original wrote it.
int decodeAdPcmBlock(int16_t* outbuf, const uint8_t* inbuf, size_t inbufsize, uint16_t channels) {
  int32_t samples = 1;
  int32_t pcmdata[2] = {};
  int8_t  index[2]   = {};

  if(inbufsize < size_t(channels)*4 || channels>2)
    return 0;

  for(int ch=0; ch<channels; ++ch) {
    *outbuf++ = int16_t(pcmdata[ch] = int16_t(inbuf[0] | (inbuf[1]<<8)));
    index[ch] = int8_t(inbuf[2]);
    // The original's own comment: "sanitize the input a little". A block whose index is out of range
    // would walk adpcmStep past its end, and a speech archive is 6000 files nobody has validated.
    if(index[ch]<0 || index[ch]>88 || inbuf[3]!=0)
      return 0;
    inbufsize -= 4;
    inbuf     += 4;
    }

  int32_t chunks = int32_t(inbufsize/(size_t(channels)*4));
  samples += chunks*8;

  while(chunks--) {
    for(int ch=0; ch<channels; ++ch) {
      for(int i=0; i<4; ++i) {
        int step = adpcmStep[index[ch]], delta = step >> 3;
        if(*inbuf & 1) delta += (step >> 2);
        if(*inbuf & 2) delta += (step >> 1);
        if(*inbuf & 4) delta += step;
        if(*inbuf & 8) delta = -delta;
        pcmdata[ch] += delta;
        index  [ch] = int8_t(index[ch] + adpcmIndex[*inbuf & 0x7]);
        index  [ch] = std::min<int8_t>(std::max<int8_t>(index[ch],0),88);
        pcmdata[ch] = std::min(std::max(pcmdata[ch],-32768),32767);
        outbuf[i*2*channels] = int16_t(pcmdata[ch]);

        step  = adpcmStep[index[ch]];
        delta = step >> 3;
        if(*inbuf & 0x10) delta += (step >> 2);
        if(*inbuf & 0x20) delta += (step >> 1);
        if(*inbuf & 0x40) delta += step;
        if(*inbuf & 0x80) delta = -delta;
        pcmdata[ch] += delta;
        index  [ch] = int8_t(index[ch] + adpcmIndex[(*inbuf >> 4) & 0x7]);
        index  [ch] = std::min<int8_t>(std::max<int8_t>(index[ch],0),88);
        pcmdata[ch] = std::min(std::max(pcmdata[ch],-32768),32767);
        outbuf[(i*2+1)*channels] = int16_t(pcmdata[ch]);
        ++inbuf;
        }
      ++outbuf;
      }
    outbuf += size_t(channels)*7;
    }
  return samples;
  }

// ------------------------------------------------------------------ WAV
//
// One forward pass over the RIFF chunks. Written here rather than reused from Engine/sound/sound.cpp
// for the same reason as the ADPCM block above, and it decodes into the one representation the mixer
// wants: interleaved int16 at the file's own rate.
struct Pcm {
  std::vector<int16_t> smp;      // interleaved
  uint32_t             rate = 0;
  uint16_t             chan = 0;
  bool                 ok   = false;
  const char*          why  = nullptr;
  };

struct FmtChunk {
  uint16_t tag        = 0;
  uint16_t channels   = 0;
  uint32_t samplesPerSec = 0;
  uint32_t bytesPerSec   = 0;
  uint16_t blockAlign    = 0;
  uint16_t bitsPerSample = 0;
  };

enum : uint16_t {
  WaveFormatPcm      = 1,
  WaveFormatImaAdpcm = 17,
  };

// 64 MiB. Eight times the largest sound in a stock Gothic II install, and a sixth of the flexible
// memory this process gets - see the data-chunk check in decodeWav.
enum : uint32_t { kMaxWavData = 64u*1024u*1024u };

bool readAll(IDevice& fin, void* dst, size_t n) {
  return fin.read(dst,n)==n;
  }

Pcm decodeWav(IDevice& fin) {
  Pcm out;

  char riff[4] = {};
  uint32_t riffSize = 0;
  char wave[4] = {};
  if(!readAll(fin,riff,4) || std::memcmp(riff,"RIFF",4)!=0) {
    out.why = "not a RIFF container";
    return out;
    }
  if(!readAll(fin,&riffSize,4) || !readAll(fin,wave,4) || std::memcmp(wave,"WAVE",4)!=0) {
    out.why = "RIFF but not WAVE";
    return out;
    }

  FmtChunk             fmt = {};
  bool                 haveFmt = false;
  std::vector<uint8_t> raw;

  for(;;) {
    char     id[4] = {};
    uint32_t sz    = 0;
    if(!readAll(fin,id,4) || !readAll(fin,&sz,4))
      break;
    if(std::memcmp(id,"fmt ",4)==0) {
      const size_t take = std::min<size_t>(sz,sizeof(fmt));
      if(!readAll(fin,&fmt,take))
        break;
      haveFmt = true;
      // The tail of an extended fmt chunk (cbSize and the ADPCM coefficient table) is skipped: the
      // IMA decoder derives everything it needs from blockAlign and channels.
      if(sz>take && fin.seek(sz-take)!=sz-take)
        break;
      } else
    if(std::memcmp(id,"data",4)==0) {
      // ⚠ `sz` COMES STRAIGHT OUT OF THE FILE, so it is a request, not a fact. A uint32 chunk header
      // can ask for 4 GiB on a console with 387 MiB of flexible memory. Gothic's own archives are the
      // only input today - the largest sound in a stock install is under 8 MB - so this is about a
      // corrupt or modded file, not a live defect. Refused by name rather than left to bad_alloc,
      // because "wav refused: data chunk larger than any sound" says which file lied.
      if(sz>kMaxWavData) {
        out.why = "data chunk larger than any sound Gothic ships";
        return out;
        }
      raw.resize(sz);
      if(sz!=0 && !readAll(fin,raw.data(),sz))
        break;
      } else {
      if(fin.seek(sz)!=sz)
        break;
      }
    // RIFF chunks are word-aligned; an odd size carries a pad byte that is not part of it.
    if((sz%2)!=0 && fin.seek(1)!=1)
      break;
    }

  if(!haveFmt) {
    out.why = "no fmt chunk";
    return out;
    }
  if(raw.empty()) {
    out.why = "no data chunk";
    return out;
    }
  if(fmt.channels==0 || fmt.channels>2 || fmt.samplesPerSec==0) {
    out.why = "unsupported channel count or rate";
    return out;
    }

  out.rate = fmt.samplesPerSec;
  out.chan = fmt.channels;

  if(fmt.tag==WaveFormatImaAdpcm || fmt.bitsPerSample==4) {
    // ⚠ blockAlign MUST CLEAR THE PREAMBLE, not merely be non-zero. The frames-per-block formula
    // below computes `blockAlign - 4*channels` in UNSIGNED arithmetic, so a header claiming a block
    // smaller than its own per-channel preamble wraps to ~4e9 frames per block and the resize()
    // that follows asks for a length no allocator will give. It is caught today only because
    // SoundDevice::load wraps the whole decode in catch(...); this says no at the header instead.
    if(fmt.blockAlign < uint16_t(fmt.channels*4u + 4u)) {
      out.why = "adpcm blockAlign smaller than one preamble plus a nibble byte per channel";
      return out;
      }
    // The frames-per-block formula is the original's: (blockAlign - 4*channels) nibble bytes carry two
    // samples each per channel, plus the one sample the preamble states outright.
    const uint32_t perBlock = (uint32_t(fmt.blockAlign)-uint32_t(fmt.channels)*4u)*
                              (uint32_t(fmt.channels)^3u)+1u;
    const uint32_t blocks   = uint32_t(raw.size())/fmt.blockAlign;
    out.smp.resize(size_t(blocks)*perBlock*fmt.channels);
    const uint8_t* src = raw.data();
    int16_t*       dst = out.smp.data();
    for(uint32_t b=0; b<blocks; ++b) {
      if(decodeAdPcmBlock(dst,src,fmt.blockAlign,fmt.channels)==0) {
        // A malformed block truncates the sound rather than failing it: half a spoken line is better
        // than a refused one, and the truncation is where the file went wrong.
        out.smp.resize(size_t(b)*perBlock*fmt.channels);
        break;
        }
      src += fmt.blockAlign;
      dst += size_t(perBlock)*fmt.channels;
      }
    out.ok = true;
    return out;
    }

  if(fmt.tag!=WaveFormatPcm) {
    out.why = "not PCM and not IMA-ADPCM";
    return out;
    }

  if(fmt.bitsPerSample==16) {
    out.smp.resize(raw.size()/2);
    std::memcpy(out.smp.data(),raw.data(),out.smp.size()*2);
    out.ok = true;
    return out;
    }
  if(fmt.bitsPerSample==8) {
    // Never seen in Gothic's archives (0 of 968 files) and handled because it costs four lines.
    // 8-bit WAV is UNSIGNED, centred on 128.
    out.smp.resize(raw.size());
    for(size_t i=0; i<raw.size(); ++i)
      out.smp[i] = int16_t((int(raw[i])-128)<<8);
    out.ok = true;
    return out;
    }

  out.why = "PCM but neither 8- nor 16-bit";
  return out;
  }

// ------------------------------------------------------------------ the mixer

enum : uint32_t {
  OutRate  = 48000,   // the only rate the main port accepts
  OutChan  = 2,
  // 5.33 ms per block. The smallest grain the port accepts, because the block period IS the output
  // latency and Gothic's footsteps and hit sounds are the kind of thing latency is heard in. Raising
  // it costs nothing else - the mix is trivially cheap next to the frame - so it is the value to move
  // first if the thread is ever found to be starved.
  OutGrain = 256,
  };

// What one voice is reading from.
struct Source {
  // Static PCM: shared with Tempest's Sound::Data so the samples outlive the SoundEffect if the same
  // Sound is played twice.
  // ⚠ ALIASED, NOT COPIED, and the comment here used to promise the first while the code did the
  // second. It said the samples were "shared with Tempest's Sound::Data ... if the same Sound is
  // played twice"; SoundEffect made a fresh vector per effect and memcpy'd into it, so every sound
  // existed TWICE - once in Sound::Data::ptr and once here - and N times over for N slots playing
  // it. Gothic ships 968 sound files and 6000 speech lines into 387 MiB of flexible memory.
  //
  // Now the voice keeps Sound::Data alive by shared_ptr and reads its buffer in place. Data already
  // was a shared_ptr; nothing but this class ever needed to own a second copy.
  // shared_ptr<void> and not shared_ptr<Sound::Data>: Data is PRIVATE to Tempest::Sound, and only
  // SoundEffect is a friend. The conversion happens in SoundEffect's constructor, where the name is
  // reachable; shared_ptr keeps the original deleter, so Data is still destroyed as Data.
  std::shared_ptr<void> hold;
  const int16_t*        smp   = nullptr;   // int16 samples, interleaved, inside `hold`
  size_t                count = 0;         // number of int16 in `smp`, not frames
  uint32_t       rate = OutRate;
  uint16_t       chan = 1;

  // ...or a producer, which is pulled instead. Never both.
  SoundProducer* producer = nullptr;
  };

struct Voice {
  Source   src;
  double   cursor   = 0.0;    // fractional frame index into src.smp
  bool     playing  = false;
  bool     finished = false;

  float    volume   = 1.f;
  // Gothic sets music and effect volume on DIFFERENT SoundDevice objects, so the device's gain is part
  // of the voice. Held by shared_ptr rather than by pointer: a device outliving its effects is the
  // normal case, the reverse is not obviously impossible, and a dangling float here would be a crackle
  // nobody could trace.
  std::shared_ptr<float> devGain;

  bool     positional = false;
  Vec3     pos;
  float    maxDist    = 0.f;

  // The pull buffer for a producer. FOUR int16 per frame, not the two a stereo producer owes:
  // OpenGothic's music providers write n*sizeof(int32_t)*2 bytes when music is disabled, twice what
  // renderSound promises (game/gamemusic.cpp). This fork's own change to that file fixes the upstream
  // defect, and this slack is what keeps the fix from being load-bearing - if that change is ever
  // dropped the write still lands inside this buffer instead of in another live allocation.
  std::vector<int16_t> pull;
  size_t   pullHave = 0;    // frames currently in `pull`; the cursor indexes it directly
  };

struct OrbisMixer {
  OrbisMixer() {
    const int32_t userRc = sceUserServiceInitialize(nullptr);
    const int32_t initRc = sceAudioOutInit();
    port = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,0,
                           OutGrain,OutRate,ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if(port<=0) {
      // Named, counted once, and NOT fatal: a title that cannot open the port must still run, and the
      // difference between "silent because there is no backend" and "silent because Open refused with
      // this code" is the whole reason this line exists.
      ps4_log("sound: sceAudioOutOpen refused 0x%08x (userService 0x%08x, init 0x%08x) - "
              "the title runs silent",uint32_t(port),uint32_t(userRc),uint32_t(initRc));
      port = -1;
      return;
      }
    int32_t vol[8] = { 32768,32768,32768,32768,32768,32768,32768,32768 };
    sceAudioOutSetVolume(port,0xFF,vol);
    ps4_log("sound: sceAudioOut port %d open - %u Hz, grain %u (%.2f ms), s16 stereo; "
            "every voice is resampled here because the main port accepts no other rate",
            int(port),uint32_t(OutRate),uint32_t(OutGrain),
            1000.0*double(OutGrain)/double(OutRate));

    std::thread(&OrbisMixer::run,this).detach();
    }

  // ⚠ THE VOICE FIELDS ARE MIXER STATE, AND EVERY WRITER MUST SAY SO. SoundEffect's setters run on
  // the game thread and write Voice::playing/finished/cursor/pos/volume while mixBlock() reads them
  // under `sync` on the mixer thread. They took no lock at all until 2026-08-21.
  //
  // Not merely formal: play() on a finished voice sets finished=false, cursor=0, playing=true, and
  // an in-flight mixStatic that started before can then write finished=true, playing=false at
  // end-of-sample - so the sound that was just started never plays. Gothic plays footsteps and hit
  // sounds from pooled SoundEffect slots, so play()-on-finished is the common path.
  //
  // Safe against recursion: the lock is held across a producer's renderSound(), and nothing
  // reachable from either of OpenGothic's producers registers, plays or pauses an effect.
  std::mutex& voiceLock() { return sync; }

  void add(Voice* v) {
    std::lock_guard<std::mutex> guard(sync);
    voice.push_back(v);
    }

  void remove(Voice* v) {
    std::lock_guard<std::mutex> guard(sync);
    for(size_t i=0; i<voice.size(); ++i)
      if(voice[i]==v) {
        voice[i] = voice.back();
        voice.pop_back();
        return;
        }
    }

  void setListener(const Vec3& p) {
    std::lock_guard<std::mutex> guard(sync);
    lisPos = p;
    }

  void setListenerDir(const Vec3& fwd, const Vec3& up) {
    // right = forward x up, normalised. The pan below is a projection onto it, so a listener whose
    // basis is degenerate (either vector zero, or the two parallel) must pan to centre rather than
    // divide by zero - which is what a NaN in a mix buffer sounds like, and it is not subtle.
    const Vec3 r = { fwd.y*up.z - fwd.z*up.y,
                     fwd.z*up.x - fwd.x*up.z,
                     fwd.x*up.y - fwd.y*up.x };
    const float len = std::sqrt(r.x*r.x + r.y*r.y + r.z*r.z);
    std::lock_guard<std::mutex> guard(sync);
    if(len>1e-6f)
      lisRight = Vec3(r.x/len,r.y/len,r.z/len);
    else
      lisRight = Vec3(0,0,0);
    }

  // One block, mixed in float and clamped once at the end. Float because summing twenty int16 voices
  // in int16 clips at every step instead of only at the output.
  void mixBlock() {
    for(uint32_t i=0; i<OutGrain*OutChan; ++i)
      acc[i] = 0.f;

    std::lock_guard<std::mutex> guard(sync);
    // The lock is held across renderSound(), and that is safe for exactly the reason the null backend
    // stated: nothing reachable from a producer's renderSound registers, unregisters, plays or pauses
    // an effect. Checked against both of OpenGothic's producers - the video widget takes its own
    // samples mutex and the music provider its own pendingSync. If a third producer ever appears, this
    // is the assumption it has to satisfy.
    for(Voice* v:voice)
      mixVoice(*v);
    // Inside the lock, and see census() for why: the loop above owns `voice` and this is the only
    // instant the counts are consistent.
    census();

    for(uint32_t i=0; i<OutGrain*OutChan; ++i) {
      const float f = acc[i];
      out[i] = int16_t(std::min(std::max(f,-32768.f),32767.f));
      }
    }

  void mixVoice(Voice& v) {
    if(!v.playing || v.finished)
      return;

    float gain = v.volume;
    if(v.devGain!=nullptr)
      gain *= *v.devGain;
    float gl = gain, gr = gain;

    if(v.positional) {
      const Vec3  d    = Vec3(v.pos.x-lisPos.x, v.pos.y-lisPos.y, v.pos.z-lisPos.z);
      const float dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
      // A LINEAR ROLLOFF TO ZERO AT maxDistance, and it is a choice rather than physics - OpenAL's
      // default is inverse-distance with a reference radius. Stated here because it is audible and
      // adjustable: sounds will carry further or less far than on the desktop build, and that is the
      // knob to turn if the world sounds wrong rather than broken.
      if(v.maxDist>0.f) {
        const float a = 1.f - dist/v.maxDist;
        gain *= (a>0.f) ? a : 0.f;
        }
      // Constant-power pan from the projection onto the listener's right vector, so a source directly
      // ahead is centred and one at 90 degrees is fully on one side without the total loudness moving.
      float p = 0.f;
      if(dist>1e-4f) {
        const float dot = (d.x*lisRight.x + d.y*lisRight.y + d.z*lisRight.z)/dist;
        p = std::min(std::max(dot,-1.f),1.f);
        }
      const float ang = (p+1.f)*0.25f*3.14159265358979323846f;  // 0..pi/2
      gl = gain*std::cos(ang);
      gr = gain*std::sin(ang);
      }

    if(v.src.producer!=nullptr)
      mixProducer(v,gl,gr);
    else
      mixStatic(v,gl,gr);
    }

  // Linear interpolation between the two frames the fractional cursor falls between. Enough for a
  // 22050 -> 48000 ratio and for the two 43344 Hz files; a polyphase filter would be measurable work
  // for an inaudible difference on this material.
  void mixStatic(Voice& v, float gl, float gr) {
    if(v.src.smp==nullptr || v.src.count==0) {
      v.finished = true;
      return;
      }
    const int16_t* s      = v.src.smp;
    const size_t   frames = v.src.count/v.src.chan;
    const double   step   = double(v.src.rate)/double(OutRate);

    for(uint32_t i=0; i<OutGrain; ++i) {
      const size_t i0 = size_t(v.cursor);
      if(i0+1>=frames) {
        v.finished = true;
        v.playing  = false;
        return;
        }
      const float  frac = float(v.cursor - double(i0));
      float        l, r;
      if(v.src.chan==1) {
        const float a = float(s[i0]), b = float(s[i0+1]);
        l = r = a + (b-a)*frac;
        } else {
        const float al = float(s[i0*2+0]),   bl = float(s[(i0+1)*2+0]);
        const float ar = float(s[i0*2+1]),   br = float(s[(i0+1)*2+1]);
        l = al + (bl-al)*frac;
        r = ar + (br-ar)*frac;
        }
      acc[i*2+0] += l*gl;
      acc[i*2+1] += r*gr;
      v.cursor += step;
      }
    }

  // A producer is pulled rather than indexed, so the cursor walks a pull buffer that is refilled when
  // it runs out. renderSound is asked for frames at the producer's OWN rate; the resample to 48000
  // happens here, exactly as for a static sound.
  //
  // THE REFILL DOES NOT SKIP AN OUTPUT FRAME, and the first version of this function did. It refilled
  // and then `continue`d, so one output frame in every block was left unwritten for this voice - a
  // dropped sample at 187 Hz, which is a buzz rather than a glitch anyone would blame on the producer.
  // Found by reading the loop rather than by hearing it, which is the only reason it never cost a flash.
  void mixProducer(Voice& v, float gl, float gr) {
    const double step = double(v.src.rate)/double(OutRate);
    // Two frames of margin: the interpolation reads i0 and i0+1, and the cursor may sit just short of
    // the last frame the previous block left behind.
    const size_t need = size_t(std::ceil(step*double(OutGrain)))+2;
    if(v.pull.size() < need*4)
      v.pull.resize(need*4);   // four int16 per frame - see Voice::pull

    for(uint32_t i=0; i<OutGrain; ++i) {
      size_t i0 = size_t(v.cursor);
      if(i0+1>=v.pullHave) {
        // Keep the frame the cursor stands on and its successor, rebase them to 0, and refill behind
        // them. Dropping them instead would discard part of a frame per block, which is audible.
        const size_t keep = (v.pullHave>i0) ? (v.pullHave-i0) : 0;
        for(size_t k=0; k<keep; ++k) {
          v.pull[k*2+0] = v.pull[(i0+k)*2+0];
          v.pull[k*2+1] = v.pull[(i0+k)*2+1];
          }
        const size_t want = (need>keep) ? (need-keep) : 0;
        // ZEROED BEFORE THE PULL, not after a check on a return value: renderSound has none, and a
        // producer with less than the whole request buffered consumes NOTHING and writes NOTHING
        // (game/ui/videowidget.cpp:91). Without this the block would replay the previous one.
        std::memset(v.pull.data()+keep*2,0,want*2*sizeof(int16_t));
        if(want>0)
          v.src.producer->renderSound(v.pull.data()+keep*2,want);
        v.pullHave = keep+want;
        v.cursor  -= double(i0);
        i0         = size_t(v.cursor);
        if(i0+1>=v.pullHave)
          return;   // cannot interpolate even after a refill: leave the rest of the block silent
        }
      const float frac = float(v.cursor - double(i0));
      const float al = float(v.pull[i0*2+0]), bl = float(v.pull[(i0+1)*2+0]);
      const float ar = float(v.pull[i0*2+1]), br = float(v.pull[(i0+1)*2+1]);
      acc[i*2+0] += (al + (bl-al)*frac)*gl;
      acc[i*2+1] += (ar + (br-ar)*frac)*gr;
      v.cursor += step;
      }
    }

  void run() {
    for(;;) {
      mixBlock();
      // THE PACING, and there is nothing else. Measured at 5332 us against 5333 us of audio per block,
      // so this call is the clock: it blocks until the port has room. A mixer that also
      // slept would drift; one that watched a clock would fight this one.
      sceAudioOutOutput(port,out);
      ++blocks;
      }
    }

  // Called from the MIXER THREAD, and the first version called it from SoundDevice::process() on the
  // strength of a comment that said OpenGothic calls that per frame. It does not call it AT ALL - `grep
  // -rn '\.process()' game/` finds nothing - so the census never fired once and a run said exactly as
  // little as before the census existed.
  //
  //   LEDGER: A CENSUS ON A CALL NOBODY MAKES IS SILENCE THAT READS AS A BROKEN BACKEND. The second
  //   time in one session that the OBSERVATION POINT was the defect rather than the thing observed -
  //   the other was a one-shot startup line the receiver was not yet listening for. Verify that the
  //   place a diagnostic lives is reached, with the same rigour as the thing it reports.
  //
  // The mixer thread is the honest place: it is the subject of the report and it always runs. It logs
  // from a second thread, which the null backend's pump deliberately never did - acceptable here
  // because ps4_log formats into a stack buffer and netlog sends one datagram per call, so the worst
  // case is two lines interleaving, not a corrupted one.
  //
  // Says what is registered and what is playing. It does NOT say whether the port is open or whether
  // this thread is running, and it cannot: both of those are conditions for it being called at all.
  // The constructor reports a refused port; a mixer thread that is not running reports itself by the
  // ABSENCE of every line below.
  //
  // ⚠ IT USED TO CLAIM BOTH, AND THE FIRST LINE OF EVERY RUN WAS FALSE. `censusAt` started at 0 and
  // this is called from mixBlock() before `++blocks`, so the first call saw blocks==0 and printed
  // "the mixer thread is not running" - from the mixer thread. Its sibling arm was unreachable in the
  // other direction: the constructor returns before spawning this thread when the port refuses, so
  // `port<=0` here was impossible and the one diagnostic written for a refused port never printed.
  void census() {
    // ~2 s at 187.5 blocks/s. Rare enough to cost nothing, often enough that a receiver started
    // mid-run still sees it. Starts at 1 rather than 0 so the first block, which has not been output
    // yet, is not reported as a block that has.
    if(blocks<censusAt)
      return;
    censusAt = uint32_t(blocks)+384;
    // The counts are read while the mix loop owns them, which is the only moment they are consistent.
    // This does not TAKE the lock: mixBlock() already holds it and it is not recursive.
    size_t total = voice.size(), playing = 0, prod = 0;
    for(const Voice* v:voice) {
      if(v->playing && !v->finished)
        ++playing;
      if(v->src.producer!=nullptr)
        ++prod;
      }
    // ⚠ ps4_log_frame AND NOT ps4_log, AND THE LOCK IS WHY. mixBlock() holds `sync` across this call,
    // and ps4_log writes klog, which orbis-compat measured at 8-15 ms a line on this console. The
    // block period is 5.33 ms - so one census line used to stall the mixer past a whole block while
    // holding the mutex, which is an underrun in the audio AND the render thread blocked in
    // setListenerPosition/add/remove for the same window, every two seconds. ps4_log_frame is UDP
    // only: one datagram, ~11 us, which is what the interval above was costed against.
    ps4_log_frame("sound: census port %d, %llu block(s) out, %zu voice(s) (%zu playing, %zu producer), "
                  "%u sound(s) decoded, %u refused",
                  int(port),(unsigned long long)blocks,total,playing,prod,nDecoded,nRefused);
    }

  int32_t     port = -1;
  // ---------------------------------------------------------------- the census
  //
  // WHY A REPEATING LINE AND NOT THE ONE AT STARTUP. The first console run of this backend produced NO
  // `sound:` line at all, and the reason was not the backend: the netlog receiver was started after the
  // title, so every line printed before it was listening went nowhere. A one-shot line at startup is
  // unobservable in exactly the situation where it matters most - a run somebody is already watching.
  //
  // So the state that decides "is there sound" is restated periodically, from the MIXER THREAD - see
  // census(), which is where the second half of that lesson is written down. Counters are plain and
  // unsynchronised: a census that costs a lock per sample would be measuring itself, and an off-by-one
  // in a count nobody adds up is not a wrong answer.
  uint64_t    blocks   = 0;   // blocks handed to sceAudioOutOutput
  uint32_t    nDecoded = 0;   // sounds decoded
  uint32_t    nRefused = 0;   // sounds refused by the decoder
  uint32_t    censusAt = 1;   // 1, not 0 - see census()
  std::mutex  sync;
  std::vector<Voice*> voice;
  Vec3        lisPos;
  Vec3        lisRight = Vec3(1,0,0);
  float       acc[OutGrain*OutChan] = {};
  int16_t     out[OutGrain*OutChan] = {};
  };

// Never destroyed, and for the same reason the null backend's was not: the thread is detached and a
// title on this console ends by having its process torn down, so a static destructor racing a running
// mixer is the one failure nobody could read out of a UDP log.
OrbisMixer& mixer() {
  static OrbisMixer* m = new OrbisMixer();
  return *m;
  }

}

// ------------------------------------------------------------------ SoundDevice

struct SoundDevice::Data {
  // shared_ptr because every voice this device creates holds one. See Voice::devGain.
  std::shared_ptr<float> gain = std::make_shared<float>(1.f);
  };

SoundDevice::SoundDevice() : data(new Data()) {
  mixer();
  }

SoundDevice::SoundDevice(std::string_view) : data(new Data()) {
  mixer();
  }

SoundDevice::~SoundDevice() = default;

std::vector<SoundDevice::Props> SoundDevice::devices() {
  // One port, one device, and no name for it: sceAudioOut enumerates nothing. An empty list is what
  // OpenGothic's settings menu already handles.
  return std::vector<Props>();
  }

SoundEffect SoundDevice::load(const char* fname) {
  try {
    Sound s(fname);
    return load(s);
    }
  catch(...) {
    // A sound that cannot be read is not a reason to end a frame. The decoder has already said why on
    // the log; this returns the empty effect every caller handles.
    return SoundEffect();
    }
  }

SoundEffect SoundDevice::load(Tempest::IDevice& d) {
  try {
    Sound s(d);
    return load(s);
    }
  catch(...) {
    return SoundEffect();
    }
  }

SoundEffect SoundDevice::load(const Sound& snd) {
  return SoundEffect(*this,snd);
  }

SoundEffect SoundDevice::load(std::unique_ptr<SoundProducer>&& p) {
  // THE PRODUCER MUST STAY ALIVE for the effect's whole lifetime, not load()'s. OpenGothic's GameMusic
  // keeps a RAW pointer taken from p.get() and calls through it long afterwards (gamemusic.h:54,
  // gamemusic.cpp:404 then :359/:363/:380). An earlier version of the null backend dropped it here and
  // the provider's own recursive_mutex was destroyed four statements before GameMusic::setEnabled()
  // locked it - which is where a console run died with EINVAL from Sony's pthread_mutex_lock.
  return SoundEffect(*this,std::move(p));
  }

void SoundDevice::process() {
  // Nothing to retire: a finished voice marks itself in the mix and its SoundEffect frees it. And
  // NOTHING IN OPENGOTHIC CALLS THIS - `grep -rn '\.process()' game/` finds nothing - which is why the
  // census does not live here. It is defined because the header declares it.
  }

void SoundDevice::suspend() {
  }

void SoundDevice::setListenerPosition(const Tempest::Vec3& p) {
  mixer().setListener(p);
  }

void SoundDevice::setListenerPosition(float x, float y, float z) {
  mixer().setListener(Vec3(x,y,z));
  }

void SoundDevice::setListenerDirection(const Tempest::Vec3& f, const Tempest::Vec3& up) {
  mixer().setListenerDir(f,up);
  }

void SoundDevice::setListenerDirection(float dx, float dy, float dz,
                                       float ux, float uy, float uz) {
  mixer().setListenerDir(Vec3(dx,dy,dz),Vec3(ux,uy,uz));
  }

void SoundDevice::setGlobalVolume(float v) {
  if(data==nullptr)
    return;
  // Same lock: mixVoice reads *devGain for every voice this device created.
  std::lock_guard<std::mutex> guard(mixer().voiceLock());
  *data->gain = std::min(std::max(v,0.f),1.f);
  }

void* SoundDevice::context() {
  return nullptr;
  }

// ------------------------------------------------------------------ Sound
//
// `Sound::Data`'s layout is fixed by the header and its `format` field was an AL_FORMAT_* enum. Nothing
// outside this TU reads it, so it carries the CHANNEL COUNT here, and `ptr` carries interleaved int16
// at `frequency`. Said out loud because the name promises otherwise.

Sound::Data::~Data() = default;

uint64_t Sound::Data::timeLength() const {
  const uint32_t chan = (format>0) ? uint32_t(format) : 1u;
  if(frequency==0 || byteSize==0)
    return 0;
  const uint64_t frames = uint64_t(byteSize)/(2u*chan);
  return frames*1000u/frequency;
  }

namespace {

// Once per distinct reason, not once per file. A Gothic install is 968 sound files and 6000 speech
// lines; one line per failure would be the whole log, and the REASON is the thing worth having - the
// same shape gnmlog.h's gnmLogOnceFor uses for shader refusals.
void sayWavRefusal(const char* why) {
  static const char* seen[8] = {};
  for(uint32_t i=0; i<8; ++i) {
    if(seen[i]==nullptr) {
      seen[i] = why;
      ps4_log("sound: wav refused: %s - reported once per reason",why);
      return;
      }
    if(seen[i]==why)
      return;
    }
  }

}

// The four path constructors go through Tempest's own file device, exactly as the OpenAL backend did;
// only the decode is ours. `format` carries the channel count - see the note above Sound::Data.
void Sound::implLoad(IDevice& fin) {
  Pcm pcm = decodeWav(fin);
  if(!pcm.ok) {
    ++mixer().nRefused;
    sayWavRefusal(pcm.why!=nullptr ? pcm.why : "unknown");
    // Thrown, because that is what every caller of a Sound constructor already handles and what the
    // OpenAL backend did. SoundDevice::load catches it and hands back the empty effect.
    throw std::runtime_error("Invalid sound file");
    }
  ++mixer().nDecoded;
  // The first one, with its format, because "sounds decode" and "sounds decode AS WHAT" are different
  // facts and the census only counts. Said once.
  static bool saidFirst = false;
  if(!saidFirst) {
    saidFirst = true;
    ps4_log("sound: first decode ok - %u Hz, %u channel(s), %zu frame(s); every voice is resampled to "
            "%u Hz because the main port accepts no other rate",
            pcm.rate,uint32_t(pcm.chan),pcm.smp.size()/std::max<uint16_t>(pcm.chan,1),
            uint32_t(OutRate));
    }
  auto d = std::make_shared<Data>();
  d->byteSize  = uint32_t(pcm.smp.size()*sizeof(int16_t));
  d->frequency = pcm.rate;
  d->format    = int32_t(pcm.chan);
  d->ptr.reset(new char[d->byteSize]);
  std::memcpy(d->ptr.get(),pcm.smp.data(),d->byteSize);
  data = d;
  }

Sound::Sound(const char* path) {
  Tempest::RFile f(path);
  implLoad(f);
  }

Sound::Sound(const std::string& path) {
  Tempest::RFile f(path);
  implLoad(f);
  }

Sound::Sound(const char16_t* path) {
  Tempest::RFile f(path);
  implLoad(f);
  }

Sound::Sound(const std::u16string& path) {
  Tempest::RFile f(path);
  implLoad(f);
  }

Sound::Sound(IDevice& f) {
  implLoad(f);
  }

bool Sound::isEmpty() const {
  return data==nullptr;
  }

uint64_t Sound::timeLength() const {
  return (data!=nullptr) ? data->timeLength() : 0;
  }

// ------------------------------------------------------------------ SoundProducer

SoundProducer::SoundProducer(uint16_t frequency, uint16_t channels)
  : frequency(frequency), channels(channels) {
  }

// ------------------------------------------------------------------ SoundEffect

struct SoundEffect::Impl {
  ~Impl() {
    if(inMixer)
      mixer().remove(&voice);
    }

  Voice voice;
  bool  inMixer = false;
  // Owned. See SoundDevice::load(unique_ptr<SoundProducer>&&).
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

// A static sound. The voice takes a SHARE of Sound::Data and reads its buffer in place - it does not
// copy. A Sound is routinely a temporary (SoundDevice::load(const char*) builds one on the stack), and
// that is exactly why the share is needed: Data outlives the Sound because everyone who needs it holds
// one, and the mixer holds this voice for as long as the effect lives.
SoundEffect::SoundEffect(SoundDevice& dev, const Sound& src)
  : impl(new Impl()) {
  if(src.data!=nullptr && src.data->byteSize>0) {
    impl->voice.src.hold  = src.data;
    impl->voice.src.smp   = reinterpret_cast<const int16_t*>(src.data->ptr.get());
    impl->voice.src.count = src.data->byteSize/sizeof(int16_t);
    impl->voice.src.rate = src.data->frequency;
    impl->voice.src.chan = uint16_t((src.data->format>0) ? src.data->format : 1);
    }
  if(dev.data!=nullptr)
    impl->voice.devGain = dev.data->gain;
  mixer().add(&impl->voice);
  impl->inMixer = true;
  }

// A producer. `frequency` and `channels` are private to SoundProducer with `friend class SoundEffect`,
// which is why the voice can only be filled in HERE and not by the mixer.
SoundEffect::SoundEffect(SoundDevice& dev, std::unique_ptr<SoundProducer>&& src)
  : impl(new Impl()) {
  impl->producer = std::move(src);
  if(impl->producer!=nullptr) {
    impl->voice.src.producer = impl->producer.get();
    impl->voice.src.rate     = impl->producer->frequency;
    // A producer's renderSound contract is int16 STEREO frames whatever `channels` says - the mixer's
    // pull buffer is interleaved two-wide - so the channel count is not used for it.
    impl->voice.src.chan     = 2;
    }
  if(dev.data!=nullptr)
    impl->voice.devGain = dev.data->gain;
  mixer().add(&impl->voice);
  impl->inMixer = true;
  }

void SoundEffect::play() {
  if(impl==nullptr)
    return;
  std::lock_guard<std::mutex> guard(mixer().voiceLock());
  // A play() on a finished voice restarts it, which is what "play this sound" means to every caller in
  // OpenGothic - it keeps a SoundEffect per slot and plays it again rather than reloading.
  if(impl->voice.finished) {
    impl->voice.finished = false;
    impl->voice.cursor   = 0.0;
    impl->voice.pullHave = 0;
    }
  impl->voice.playing = true;
  }

void SoundEffect::pause() {
  if(impl==nullptr)
    return;
  std::lock_guard<std::mutex> guard(mixer().voiceLock());
  impl->voice.playing = false;
  }

bool SoundEffect::isEmpty() const {
  if(impl==nullptr)
    return true;
  return impl->voice.src.smp==nullptr && impl->voice.src.producer==nullptr;
  }

bool SoundEffect::isFinished() const {
  if(impl==nullptr)
    return true;
  // A producer never finishes: music and a video's audio track end when their owner drops the effect,
  // and a producer that reported finished would have its slot retired under it. A static sound
  // finishes when the mix runs off the end of its samples.
  if(impl->voice.src.producer!=nullptr)
    return false;
  return impl->voice.finished || impl->voice.src.smp==nullptr;
  }

uint64_t SoundEffect::timeLength() const {
  if(impl==nullptr || impl->voice.src.smp==nullptr)
    return 0;
  const size_t frames = impl->voice.src.count/std::max<uint16_t>(impl->voice.src.chan,1);
  return uint64_t(frames)*1000u/std::max<uint32_t>(impl->voice.src.rate,1u);
  }

uint64_t SoundEffect::currentTime() const {
  if(impl==nullptr)
    return 0;
  // Locked, unlike volume() and position() below: those read what the game thread itself wrote,
  // while `cursor` is written by the MIXER thread on every block.
  //
  // ⚠ MEANINGFUL ONLY FOR A STATIC SOUND. For a producer the cursor indexes the pull buffer and is
  // rebased to near zero on every refill (mixProducer), so this reports a time near zero forever.
  // Nothing in OpenGothic calls it - `grep -rn 'currentTime()' game/` finds no call site at all -
  // so it is defined because the header declares it, like SoundDevice::process() beside it.
  std::lock_guard<std::mutex> guard(mixer().voiceLock());
  return uint64_t(impl->voice.cursor)*1000u/std::max<uint32_t>(impl->voice.src.rate,1u);
  }

void SoundEffect::setPosition(const Tempest::Vec3& p) {
  if(impl==nullptr)
    return;
  std::lock_guard<std::mutex> guard(mixer().voiceLock());
  impl->voice.pos        = p;
  impl->voice.positional = true;
  }

void SoundEffect::setPosition(float x, float y, float z) {
  setPosition(Vec3(x,y,z));
  }

void SoundEffect::setMaxDistance(float dist) {
  if(impl==nullptr)
    return;
  std::lock_guard<std::mutex> guard(mixer().voiceLock());
  impl->voice.maxDist = dist;
  }

void SoundEffect::setVolume(float v) {
  if(impl==nullptr)
    return;
  std::lock_guard<std::mutex> guard(mixer().voiceLock());
  impl->voice.volume = std::min(std::max(v,0.f),1.f);
  }

float SoundEffect::volume() const {
  return impl!=nullptr ? impl->voice.volume : 0.f;
  }

Tempest::Vec3 SoundEffect::position() const {
  return impl!=nullptr ? impl->voice.pos : Vec3();
  }

float SoundEffect::x() const { return position().x; }
float SoundEffect::y() const { return position().y; }
float SoundEffect::z() const { return position().z; }

#endif // !OG_SOUND_NULL

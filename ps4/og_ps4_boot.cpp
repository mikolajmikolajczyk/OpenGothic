#include "og_ps4_boot.h"

#include <orbis_boot.h>
#include <orbis_mem.h>
#include <orbis_stat.h>

#include "ps4_app.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <filesystem>
#include <system_error>
#include <cctype>
#include <clocale>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <system_error>

namespace Ps4Og {

// Storage the console's owner can reach. /data is the HDD area homebrew is given,
// /mnt/usbN is a mounted USB stick (the PS4 mounts up to eight), /app0 is the package
// itself and is last for the reason the header gives.
static const char* const kOgDataBases[] = {
  "/data",
  "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
  "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
  "/app0",
  };

// Directory names an installation plausibly sits under. The empty string means "the
// base itself is the root", which is what /app0 needs under the emulator and what a
// USB stick with the game unpacked at its top level needs.
static const char* const kOgDataNames[] = {
  "",
  "gothic2", "Gothic2", "GOTHIC2",
  "GothicII", "Gothic II",
  "gothic", "OpenGothic",
  };

// The two directories that make a Gothic II installation, each with the case variants
// a case-preserving-but-insensitive USB filesystem may hand back. The console's own
// /data is case-SENSITIVE, so this is not redundant.
static const char* const kDataDirNames[] = { "Data", "data", "DATA" };
static const char* const kWorkDirNames[] = { "_work", "_Work", "_WORK" };

static bool isDir(const std::string& path) {
  struct stat st = {};
  if(stat(path.c_str(),&st)!=0)
    return false;
  return (st.st_mode & S_IFMT)==S_IFDIR;
  }

static bool anyCaseDir(const std::string& root, const char* const* names, size_t n,
                       std::string& hit) {
  for(size_t i=0; i<n; ++i) {
    std::string p = root + names[i];
    if(isDir(p)) {
      hit = names[i];
      return true;
      }
    }
  return false;
  }

// Trailing '/' is what CommandLine wants and what makes every concatenation below
// a plain append.
static std::string withSlash(std::string p) {
  while(p.size()>1 && p.back()=='/')
    p.pop_back();
  p.push_back('/');
  return p;
  }

// Cheap half of CommandLine::validateGothicPath(): Data/ and _work/ both present.
static bool looksLikeGothic(const std::string& root, std::string& why) {
  std::string d, w;
  const bool hasData = anyCaseDir(root,kDataDirNames,sizeof(kDataDirNames)/sizeof(*kDataDirNames),d);
  const bool hasWork = anyCaseDir(root,kWorkDirNames,sizeof(kWorkDirNames)/sizeof(*kWorkDirNames),w);
  if(hasData && hasWork) {
    why = d + " + " + w;
    return true;
    }
  if(!isDir(root)) {
    why = "no such directory";
    return false;
    }
  if(!hasData && !hasWork)
    why = "directory exists, but no Data/ and no _work/";
  else if(!hasData)
    why = "directory exists, has _work/, but no Data/";
  else
    why = "directory exists, has Data/, but no _work/";
  return false;
  }

// ------------------------------------------------------------------ config

struct Cfg {
  std::string              root;
  std::vector<std::string> args;
  bool                     teeLog = true;
  // `probes=1`. Off by default - see the call site in boot().
  bool                     probes = false;
  bool                     present = false;
  };

static void trim(std::string& s) {
  size_t b = 0, e = s.size();
  while(b<e && (s[b]==' ' || s[b]=='\t' || s[b]=='\r'))
    ++b;
  while(e>b && (s[e-1]==' ' || s[e-1]=='\t' || s[e-1]=='\r'))
    --e;
  s = s.substr(b,e-b);
  }

// Same shape as orbis-compat's optional/ps4_app.cpp reader, and deliberately as dumb: an absent
// file is the normal case and is not an error.
static Cfg readCfg() {
  Cfg cfg;
  FILE* f = fopen("/app0/opengothic.cfg","rb");
  if(f==nullptr)
    return cfg;
  cfg.present = true;

  char line[512] = {};
  while(fgets(line,sizeof(line),f)!=nullptr) {
    std::string s = line;
    const size_t nl = s.find('\n');
    if(nl!=std::string::npos)
      s.resize(nl);
    const size_t hash = s.find('#');
    if(hash!=std::string::npos)
      s.resize(hash);
    trim(s);
    if(s.empty())
      continue;
    const size_t eq = s.find('=');
    if(eq==std::string::npos)
      continue;
    std::string key = s.substr(0,eq);
    std::string val = s.substr(eq+1);
    trim(key);
    trim(val);
    if(key=="gothic-root")
      cfg.root = val;
    else if(key=="arg")
      cfg.args.push_back(val);
    else if(key=="probes")
      cfg.probes = !(val=="0" || val=="false");
    else if(key=="log")
      cfg.teeLog = !(val=="0" || val=="false");
    else
      ps4_log("boot: opengothic.cfg - unknown key '%s', ignored",key.c_str());
    }
  fclose(f);
  return cfg;
  }

// ------------------------------------------------------------------ dirent probe

void probeDirent(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if(d==nullptr) {
    ps4_log("boot: readdir probe - opendir('%s') failed, errno %d",dir.c_str(),errno);
    return;
    }
  unsigned total = 0, dirs = 0, regs = 0, unknown = 0, other = 0;
  const char* firstDir = nullptr;
  static char firstDirBuf[256] = {};
  while(dirent* e = readdir(d)) {
    ++total;
    switch(e->d_type) {
      case DT_DIR:
        ++dirs;
        if(firstDir==nullptr && std::strcmp(e->d_name,".")!=0 && std::strcmp(e->d_name,"..")!=0) {
          std::snprintf(firstDirBuf,sizeof(firstDirBuf),"%s",e->d_name);
          firstDir = firstDirBuf;
          }
        break;
      case DT_REG:     ++regs;    break;
      case DT_UNKNOWN: ++unknown; break;
      default:         ++other;   break;
      }
    }
  closedir(d);
  ps4_log("boot: readdir probe '%s': %u entries - DT_DIR %u, DT_REG %u, DT_UNKNOWN %u, other %u",
          dir.c_str(),total,dirs,regs,unknown,other);
  if(firstDir!=nullptr)
    ps4_log("boot: readdir probe - first subdirectory reported as DT_DIR: '%s'",firstDir);
  if(unknown>0)
    ps4_log("boot: readdir probe - WARNING %u entries are DT_UNKNOWN. Tempest::Dir::scan "
            "classifies on d_type alone and has no stat() fallback, so every path under "
            "this root that OpenGothic resolves case-insensitively will fail.",unknown);
  if(dirs==0 && total>0)
    ps4_log("boot: readdir probe - WARNING this filesystem reported NO directory entries "
            "at all; Data/ and _work/ cannot be resolved through Dir::scan.");
  }

// ------------------------------------------------------------------ vdf read probe

static void hex16(char out[64], const unsigned char* p, size_t n) {
  size_t o = 0;
  for(size_t i=0; i<n && o+3<64; ++i)
    o += size_t(std::snprintf(out+o,64-o,"%02x ",p[i]));
  if(o>0)
    out[o-1] = '\0';
  else
    out[0] = '\0';
  }

// Pick a .vdf to test with: SystemPack.vdf is the one the console failed on, and it is
// the smallest archive in a stock install (64 KiB), so it is the cheapest witness too.
static bool findVdf(const std::string& root, std::string& out) {
  static const char* const kDataDirs[] = { "Data", "data", "DATA" };
  for(const char* d : kDataDirs) {
    std::string dir = root + d + "/";
    if(!isDir(dir))
      continue;
    // The named favourite first, then whatever the directory offers.
    for(const char* n : { "SystemPack.vdf", "Systempack.vdf", "SYSTEMPACK.VDF" }) {
      struct stat st = {};
      std::string p = dir + n;
      if(stat(p.c_str(),&st)==0 && (st.st_mode & S_IFMT)==S_IFREG) {
        out = p;
        return true;
        }
      }
    if(DIR* dp = opendir(dir.c_str())) {
      while(dirent* e = readdir(dp)) {
        const size_t len = std::strlen(e->d_name);
        if(len>4 && (std::strcmp(e->d_name+len-4,".vdf")==0 ||
                     std::strcmp(e->d_name+len-4,".VDF")==0)) {
          out = dir + e->d_name;
          closedir(dp);
          return true;
          }
        }
      closedir(dp);
      }
    }
  return false;
  }

void probeVdfRead(const std::string& root) {
  std::string path;
  if(!findVdf(root,path)) {
    ps4_log("boot: vdf probe - no .vdf found under %sData/, skipping",root.c_str());
    return;
    }

  const int fd = open(path.c_str(),O_RDONLY);
  if(fd<0) {
    ps4_log("boot: vdf probe - open('%s') failed, errno %d",path.c_str(),errno);
    return;
    }

  // (1) fstat, THREE ways, because this is the field that broke the port.
  //
  // The kernel's own bytes first: quadwords at 0x48 and 0x50. The uncorrected SDK
  // `struct stat` reads its st_size from 0x50, which the kernel filled with st_blocks -
  // that is the console's "st_size=128" for a 64857-byte file. 0x48 is where the real
  // size should be, and printing BOTH is what turns the layout note in the overlay's stat interposer
  // from a reconstruction into a measurement.
  unsigned char raw[orbis::kSonyStatSize] = {};
  const int     rcr = orbis::rawKernelFstat(fd,raw);
  const int64_t q48 = orbis::sonyStatQword(raw,orbis::kSonyStatSizeOff);
  const int64_t q50 = orbis::sonyStatQword(raw,orbis::kSonyStatBlocksOff);
  ps4_log("boot: vdf probe '%s': kernel bytes rc=%d raw@0x48=%lld (size) raw@0x50=%lld (blocks)",
          path.c_str(),rcr,(long long)q48,(long long)q50);

  // Then fstat() as the title now sees it - through the interposition in
  // orbis-compat's src/orbis_stat.cpp.
  struct stat st = {};
  const int    rcs = fstat(fd,&st);
  ps4_log("boot: vdf probe - fstat(corrected) rc=%d st_size=%lld st_mode=0%o st_blocks=%lld",
          rcs,(long long)st.st_size,(unsigned)st.st_mode,(long long)st.st_blocks);
  ps4_log("boot: vdf probe - uncorrected SDK layout would have reported st_size=%lld",
          (long long)q50);

  if(rcs==0 && st.st_size==q48 && q48>0)
    ps4_log("boot: vdf probe - VERDICT: stat interposition ACTIVE and correct "
            "(st_size %lld == kernel raw@0x48).",(long long)q48);
  else if(rcs==0 && st.st_size==q50)
    ps4_log("boot: vdf probe - VERDICT: stat interposition DID NOT TAKE - fstat still "
            "returns the 0x50 quadword. Check symbol resolution.");
  else if(rcs!=0 || st.st_size<=0)
    ps4_log("boot: vdf probe - VERDICT: fstat still reports no usable size. ZenKit maps "
            "st_size bytes and does not check it, so this IS the empty signature.");

  // (2) plain read(). This is what the non-mmap path in zenkit does.
  unsigned char rbuf[16] = {};
  const ssize_t nr = read(fd,rbuf,sizeof(rbuf));
  char rhex[64] = {};
  hex16(rhex,rbuf,nr>0 ? size_t(nr) : 0u);
  ps4_log("boot: vdf probe - read() returned %lld, first bytes: %s",(long long)nr,rhex);

  // (3) file-backed mmap, exactly the call zenkit's MmapPosix.cc makes (one page is
  // enough - the signature lives in the first 16 bytes).
  const size_t mlen = (st.st_size>0 && size_t(st.st_size)<4096u) ? size_t(st.st_size) : 4096u;
  void* m = mmap(nullptr,mlen,PROT_READ,MAP_SHARED,fd,0);
  if(m==MAP_FAILED) {
    ps4_log("boot: vdf probe - mmap(MAP_SHARED, fd) returned MAP_FAILED, errno %d",errno);
    ps4_log("boot: vdf probe - VERDICT: mmap refuses a file fd here. NOTE zenkit compares "
            "the result against nullptr, not MAP_FAILED (src/MmapPosix.cc), so it would "
            "have used -1 as a pointer.");
    } else {
    unsigned char mbuf[16] = {};
    std::memcpy(mbuf,m,sizeof(mbuf)<mlen ? sizeof(mbuf) : mlen);
    char mhex[64] = {};
    hex16(mhex,mbuf,sizeof(mbuf));
    ps4_log("boot: vdf probe - mmap() ok at %p, first bytes: %s",m,mhex);
    if(nr>0 && std::memcmp(mbuf,rbuf,size_t(nr)<16u ? size_t(nr) : 16u)==0)
      ps4_log("boot: vdf probe - VERDICT: mmap agrees with read(). File-backed mmap WORKS "
              "here, which is what the console measured on 2026-08-02 - mmap was never "
              "the fault, the size handed to it was.");
    else
      ps4_log("boot: vdf probe - VERDICT: mmap DISAGREES with read(). The mapping is not "
              "backed by the file. This contradicts the console run of 2026-08-02 and "
              "ZenKit's mmap path (ZK_ENABLE_MMAP) must come back off.");
    munmap(m,mlen);
    }
  close(fd);

  // NOTE: a probe that mapped the LARGEST archive used to live here. It was removed after
  // it proved its point and then became a hazard: it took 37 s on the console to touch
  // THREE bytes of Speech1.vdf, which is 722 MB at HDD speed and is the measurement that
  // established PS4 mmap POPULATES EAGERLY. Keeping it cost 689 MiB of pointless I/O on
  // every boot, on a console that hard-hung from exactly this kind of pressure.
  //
  // ...and again on the LARGEST archive. Everything above is measured on SystemPack.vdf,
  // 64 KiB, which proves nothing about a 300 MB mapping - and ZenKit maps every archive
  // whole. This matters because Vfs::mount_disk SILENTLY DROPS any file entry whose data
  // lies past the end of the buffer (lib/ZenKit/src/Vfs.cc, the `e_offset + e_size > size`
  // check): a short mapping therefore mounts without a single error and simply loses the
  // tail of the archive. In Textures.vdf every font sits at offset ~16 MB of 318 MB, so a
  // mapping short by any amount past that takes the whole user interface with it and says
  // nothing.
  //
  // ⚠ AND NOTHING COUNTS THE DROPS. A patch that did was lost when the port left its patch queue;
  // the ZenKit fork carries no such summary, checked. So a short mapping is silent at BOTH ends -
  // neither this probe nor the mount reports it. Restoring the count is the cheaper half, and it
  // belongs in ZenKit's mount rather than in a probe here.
  }

// ------------------------------------------------------------------ data inventory

// Archives without which the UI cannot paint. Measured across all 15 archives of a stock
// NotR install: these two carry every .FNT and nothing else does.
static const char* const kFontArchives[] = { "Textures.vdf", "Textures_Addon.vdf" };

static bool fileSize(const std::string& path, long long& out) {
  struct stat st = {};
  if(stat(path.c_str(),&st)!=0)
    return false;
  if((st.st_mode & S_IFMT)!=S_IFREG)
    return false;
  out = (long long)st.st_size;
  return true;
  }

static bool endsWithNoCase(const char* name, const char* suffix) {
  const size_t n = std::strlen(name), s = std::strlen(suffix);
  if(n<s)
    return false;
  for(size_t i=0; i<s; ++i) {
    char a = name[n-s+i], b = suffix[i];
    if('A'<=a && a<='Z') a = char(a-'A'+'a');
    if('A'<=b && b<='Z') b = char(b-'A'+'a');
    if(a!=b)
      return false;
    }
  return true;
  }

// See og_ps4_boot.h for why this exists and what each of its three questions decides.
void probeSavePaths(const std::string& root) {
  ps4_log("boot: ---- save paths");

  // 1. WHAT THE WORKING DIRECTORY IS. Everything OpenGothic does with a savegame resolves against
  //    this, and nothing in this project has ever printed it. `chdir` is refused here, so whatever
  //    this says is what the title is stuck with.
  {
  char cwd[512] = {};
  if(getcwd(cwd,sizeof(cwd)-1)!=nullptr)
    ps4_log("boot: save cwd = '%s'  <- every bare 'save_slot_N.sav' resolves against THIS",cwd);
  else
    ps4_log("boot: save cwd = <getcwd failed, errno %d> - a relative path has no defined meaning "
            "in this process",errno);
  }

  // 2 and 3. The round trip, per candidate. A create that succeeds proves nothing on its own: the
  //    write, the stat's size, the read-back's CONTENT and the unlink are each a separate way for a
  //    filesystem to refuse, and the whole point is to find the root that survives all of them.
  //
  //    The bare relative name goes FIRST and is the one that matters: if it works, OpenGothic needs
  //    no patch at all and this probe has saved the work rather than started it.
  const std::string cand[] = {
    "save_slot_probe.sav",          // exactly what the game will ask for
    "/data/OpenGothic/probe.sav",   // a directory of our own, which needs mkdir to work
    "/data/probe.sav",              // the area GoldHEN gives homebrew, no mkdir needed
    // ⚠ `root + "probe.sav"` WAS HERE AND IS GONE. It wrote a file into the player's own Gothic II
    // installation and unlinked it again - and if the unlink failed, said so only on a log nobody
    // reads. Writing into someone else's game data to learn something already known is not worth it.
    "/app0/probe.sav",              // THE NEGATIVE CONTROL: the package, which must refuse
    };

  const char  payload[] = "tempest-save-probe";
  std::string winner;

  for(const std::string& path : cand) {
    // mkdir every parent component that is not the root itself. ENOENT from open(O_CREAT) and
    // "the directory does not exist" are the same errno, so the directory is made first and its
    // own result is reported - otherwise a missing directory reads as an unwritable one.
    const size_t slash = path.rfind('/');
    if(slash!=std::string::npos && slash>0) {
      const std::string dir = path.substr(0,slash);
      if(mkdir(dir.c_str(),0777)==0)
        ps4_log("boot: save   mkdir('%s') created",dir.c_str());
      else if(errno==EEXIST)
        ps4_log("boot: save   mkdir('%s') already there",dir.c_str());
      else
        ps4_log("boot: save   mkdir('%s') failed, errno %d",dir.c_str(),errno);
      }

    const int fd = open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666);
    if(fd<0) {
      ps4_log("boot: save '%s' -> open(O_CREAT) failed, errno %d",path.c_str(),errno);
      continue;
      }
    const ssize_t wr = write(fd,payload,sizeof(payload));
    const int     wrErrno = errno;
    close(fd);
    if(wr!=ssize_t(sizeof(payload))) {
      // The outcome a probe that stopped at open() would have called success.
      ps4_log("boot: save '%s' -> OPENED BUT write() returned %zd of %zu, errno %d",
              path.c_str(),wr,sizeof(payload),wrErrno);
      unlink(path.c_str());
      continue;
      }

    // The stat goes through the overlay's interposer (orbis_stat.cpp), which is the thing that
    // makes st_size mean st_size on this SDK - so a wrong size here is a regression in THAT and not
    // in the write.
    struct stat st = {};
    const bool  statOk = (stat(path.c_str(),&st)==0);
    const int   statErrno = errno;

    char back[64] = {};
    ssize_t rd = -1;
    const int rfd = open(path.c_str(),O_RDONLY);
    if(rfd>=0) {
      rd = read(rfd,back,sizeof(back)-1);
      close(rfd);
      }
    const bool same = (rd==ssize_t(sizeof(payload)) && std::memcmp(back,payload,sizeof(payload))==0);

    ps4_log("boot: save '%s' -> write %zu B ok, stat %s size %lld, read-back %zd B %s",
            path.c_str(),sizeof(payload),
            statOk ? "ok" : "FAILED",
            statOk ? (long long)st.st_size : (long long)statErrno,
            rd, same ? "IDENTICAL" : "DIFFERS");

    if(unlink(path.c_str())!=0)
      ps4_log("boot: save   unlink('%s') failed, errno %d - a save would accumulate here",
              path.c_str(),errno);

    if(same && statOk && st.st_size==(off_t)sizeof(payload) && winner.empty())
      winner = path;
    }

  if(winner.empty())
    ps4_log("boot: save VERDICT: NO candidate survived create+write+stat+read - saving cannot work "
            "until one does, and the errnos above say which step refused");
  else if(winner==cand[0])
    ps4_log("boot: save VERDICT: the BARE RELATIVE name works - '%s' round-tripped, so OpenGothic's "
            "own save path needs no interposition at all",winner.c_str());
  else
    ps4_log("boot: save VERDICT: use '%s'. The bare relative name did NOT round-trip, so the six "
            "'save_slot_N.sav' sites need this prefix - and `stat` needs it too, because "
            "FileUtil::exists is a stat of the same relative name",winner.c_str());
  }

void probeDataInventory(const std::string& root) {
  ps4_log("boot: ---- installation inventory ----------------------------------");

  // The root, by name. "5 entries, DT_DIR 5" is not actionable; a list is.
  if(DIR* d = opendir(root.c_str())) {
    std::string line;
    while(dirent* e = readdir(d)) {
      if(std::strcmp(e->d_name,".")==0 || std::strcmp(e->d_name,"..")==0)
        continue;
      line += e->d_name;
      line += (e->d_type==DT_DIR) ? "/ " : " ";
      }
    closedir(d);
    ps4_log("boot: root %s contains: %s",root.c_str(),line.empty() ? "(nothing)" : line.c_str());
    }

  // Data/, with sizes, because "present" and "fully transferred" are different claims.
  std::string dataDir;
  for(const char* d : kDataDirNames) {
    std::string p = root + d + "/";
    if(isDir(p)) {
      dataDir = p;
      break;
      }
    }
  if(dataDir.empty()) {
    ps4_log("boot: NO Data/ DIRECTORY under %s - nothing can load.",root.c_str());
    ps4_log("boot: ---------------------------------------------------------------");
    return;
    }

  unsigned  vdfCount = 0;
  long long vdfBytes = 0;
  if(DIR* d = opendir(dataDir.c_str())) {
    while(dirent* e = readdir(d)) {
      if(std::strcmp(e->d_name,".")==0 || std::strcmp(e->d_name,"..")==0)
        continue;
      long long sz = 0;
      const bool isFile = fileSize(dataDir+e->d_name,sz);
      if(endsWithNoCase(e->d_name,".vdf") || endsWithNoCase(e->d_name,".mod")) {
        ++vdfCount;
        vdfBytes += sz;
        ps4_log("boot:   archive %-24s %10lld B",e->d_name,sz);
        } else {
        ps4_log("boot:   other   %-24s %10lld B%s",e->d_name,sz,isFile ? "" : "  (directory)");
        }
      }
    closedir(d);
    }
  ps4_log("boot: %u archive(s) in %s totalling %lld B",vdfCount,dataDir.c_str(),vdfBytes);

  // The verdict that matters. A missing font archive is otherwise SILENT: no archive
  // errors, a window, a swapchain, frames presenting - and a black screen.
  bool missing = false;
  for(const char* a : kFontArchives) {
    long long sz = 0;
    std::string hit;
    // Try the exact name first, then the case variants a FAT/exFAT copy may present.
    bool found = fileSize(dataDir+a,sz);
    if(!found) {
      std::string upper = a, lower = a;
      for(char& c : upper) if('a'<=c && c<='z') c = char(c-'a'+'A');
      for(char& c : lower) if('A'<=c && c<='Z') c = char(c-'A'+'a');
      found = fileSize(dataDir+upper,sz) || fileSize(dataDir+lower,sz);
      }
    if(found) {
      ps4_log("boot:   FONT ARCHIVE %s present (%lld B)",a,sz);
      } else {
      missing = true;
      ps4_log("boot:   FONT ARCHIVE %s IS MISSING",a);
      }
    }
  if(missing) {
    ps4_log("boot: ***************************************************************");
    ps4_log("boot: Every .FNT in a stock install lives in Textures.vdf and");
    ps4_log("boot: Textures_Addon.vdf - in no other archive (checked across all 15).");
    ps4_log("boot: One of them is absent here, so font lookups will fail. NOTE this is");
    ps4_log("boot: a sufficient cause, not the only one: a font can also fail to open");
    ps4_log("boot: with the archive PRESENT, if the mapping is short and drops the");
    ps4_log("boot: entries past its end. NOTHING REPORTS THAT TODAY - the counter that");
    ps4_log("boot: did was lost with the patch queue, so rule it out by hand.");
    ps4_log("boot: Copy them into %s",dataDir.c_str());
    ps4_log("boot: ***************************************************************");
    }

  // THE GATE THAT DECIDES WHETHER ANY ARCHIVE IS MOUNTED AT ALL.
  //
  // Resources::detectVdf enumerates Data/, calls vdfTimestamp() on each archive, and then
  // admits it to the mount list on exactly one condition:
  //     if(std::filesystem::file_size(ar.name)>0) ret.emplace_back(...);
  // (game/resources.cpp:269-270). If that returns 0 the archive is dropped with NO log and
  // NO exception, the mount loop body never runs, and the VFS stays empty - which is
  // indistinguishable, in every log we have, from "all 15 archives mounted fine". Every
  // subsequent resource lookup then returns nullptr, which is what
  // `failed to open resource: font_old_20_white.fnt` reports.
  //
  // This matters here because std::filesystem goes through stat(), and stat() on this
  // platform is the overlay's (orbis_stat.cpp). So the interposition that fixed the archives is
  // also the code that could silently un-mount them. Compare all three answers side by
  // side and let the numbers say which.
  {
    std::string biggest;
    long long   biggestSz = 0;
    if(DIR* d = opendir(dataDir.c_str())) {
      while(dirent* e = readdir(d)) {
        long long sz = 0;
        if(!endsWithNoCase(e->d_name,".vdf"))
          continue;
        if(fileSize(dataDir+e->d_name,sz) && sz>biggestSz) {
          biggestSz = sz;
          biggest   = e->d_name;
          }
        }
      closedir(d);
      }
    if(!biggest.empty()) {
      const std::string path = dataDir + biggest;
      struct stat st = {};
      const int    rcs = stat(path.c_str(),&st);
      std::error_code ec1, ec2;
      const auto fsSize = std::filesystem::file_size(path,ec1);
      const bool  fsReg = std::filesystem::is_regular_file(path,ec2);
      ps4_log("boot: detectVdf gate on '%s':",biggest.c_str());
      ps4_log("boot:   stat()                    rc=%d st_size=%lld st_mode=0%o",
              rcs,(long long)st.st_size,(unsigned)st.st_mode);
      ps4_log("boot:   filesystem::file_size     %llu (ec=%d '%s')",
              (unsigned long long)fsSize,ec1.value(),ec1.message().c_str());
      ps4_log("boot:   filesystem::is_regular    %d (ec=%d)",fsReg ? 1 : 0,ec2.value());
      if(ec1 || fsSize==0)
        ps4_log("boot:   VERDICT: THE GATE FAILS. detectVdf drops every archive here, so "
                "NOTHING is mounted and every resource lookup misses. This is the font "
                "failure, and it is upstream of ZenKit entirely.");
      else
        ps4_log("boot:   VERDICT: gate passes - archives reach the mount list.");
      }
  }
  ps4_log("boot: ---------------------------------------------------------------");
  }

// ------------------------------------------------------------------ ctype probe, crash handlers
//
// MOVED to orbis-compat on 2026-08-19: orbis::probeCtype() and orbis::installCrashHandlers().
// Neither was about Gothic - one asks whether this musl folds case, the other keeps a dying title
// from dying quietly - and the overlay is what supplies backtrace(3) for the second one to grow
// into. See <orbis_boot.h>; the fatal channel and the after-a-crash policy are registered by
// Tempest's ps4_app_init through orbis_set_log_fatal / orbis_set_fatal_action.

// ------------------------------------------------------------------ boot

Boot boot() {
  // Before the first line of work, because every later census is read against it and a
  // baseline taken after the archives are open measures the wrong thing.
  orbis::memCensusBaseline();

  Boot out;
  const Cfg cfg = readCfg();
  out.teeLog = cfg.teeLog;

  if(cfg.present)
    ps4_log("boot: read /app0/opengothic.cfg (root='%s', %zu extra arg(s), log=%d)",
            cfg.root.c_str(),cfg.args.size(),cfg.teeLog ? 1 : 0);
  else
    ps4_log("boot: no /app0/opengothic.cfg - searching the console's storage");

  // A list of everything considered, so a refusal can name all of it.
  std::vector<std::string> tried;
  std::string              accepted;

  if(!cfg.root.empty()) {
    // Explicit answer: taken or refused on its own merits, never fallen through.
    const std::string root = withSlash(cfg.root);
    std::string why;
    tried.push_back(root + "   (opengothic.cfg gothic-root)");
    if(looksLikeGothic(root,why)) {
      accepted = root;
      ps4_log("boot: gothic-root from opengothic.cfg accepted: %s (%s)",root.c_str(),why.c_str());
      } else {
      ps4_log("boot: gothic-root from opengothic.cfg REJECTED: %s - %s",root.c_str(),why.c_str());
      }
    } else {
    for(const char* base : kOgDataBases) {
      // One stat per base before eight per base: a console with no USB stick in it
      // should not print eighty lines about /mnt/usb3.
      const bool baseExists = isDir(base);
      for(const char* name : kOgDataNames) {
        std::string root = base;
        if(name[0]!='\0') {
          root.push_back('/');
          root += name;
          }
        root = withSlash(root);
        tried.push_back(root);
        if(!baseExists)
          continue;
        std::string why;
        if(looksLikeGothic(root,why)) {
          accepted = root;
          ps4_log("boot: candidate %s ACCEPTED (%s)",root.c_str(),why.c_str());
          break;
          }
        ps4_log("boot: candidate %s - %s",root.c_str(),why.c_str());
        }
      if(!accepted.empty())
        break;
      if(!baseExists)
        ps4_log("boot: base %s does not exist - %zu candidate(s) under it skipped",
                base,sizeof(kOgDataNames)/sizeof(*kOgDataNames));
      }
    }

  if(accepted.empty()) {
    ps4_log("boot: ============================================================");
    ps4_log("boot: NO GOTHIC II INSTALLATION FOUND. This title ships no game data");
    ps4_log("boot: and never will - Gothic II is not redistributable. Copy an");
    ps4_log("boot: installation (the directory containing Data/ and _work/) onto");
    ps4_log("boot: the console's own storage, or name it in /app0/opengothic.cfg:");
    ps4_log("boot:     gothic-root=/mnt/usb0/gothic2");
    ps4_log("boot: %zu path(s) were searched, in this order:",tried.size());
    for(size_t i=0; i<tried.size(); ++i)
      ps4_log("boot:   [%02zu] %s",i,tried[i].c_str());
    ps4_log("boot: ============================================================");
    return out;
    }

  ps4_log("boot: ============================================================");
  ps4_log("boot: GAME DATA ROOT: %s",accepted.c_str());
  ps4_log("boot: (searched %zu path(s) to get here)",tried.size());
  if(accepted.compare(0,6,"/app0/")==0)
    ps4_log("boot: NOTE the root is inside /app0. On a console that is the PACKAGE, "
            "which must never contain game data; this is the development layout an "
            "emulator's union mount produces.");
  ps4_log("boot: ============================================================");
  // ⚠ ONE PROBE ALWAYS, THE REST ON REQUEST. probeVdfRead touches a single 64 KiB archive and its
  // VERDICT line asserts that the stat interposition is live - a build where that silently stopped
  // resolving says so in one line here instead of failing 800 lines later as an unreadable archive.
  // That is regression armour and it is cheap.
  //
  // The other four are not. Together they emit 40-60 ps4_log lines, and klog is 8-15 ms a line on
  // this console: half a second to nearly a second added to EVERY launch, to re-answer questions
  // that were settled weeks ago. probeSavePaths also has side effects that outlive it - it creates
  // /data/OpenGothic and writes five probe files - so it is not merely slow, it is a boot that
  // changes the console's storage every time.
  //
  // `probes=1` in /app0/opengothic.cfg brings them back when something is actually being diagnosed.
  probeVdfRead(accepted);
  if(cfg.probes) {
    ps4_log("boot: probes=1 - running the full probe suite (adds ~1 s to this launch)");
    probeDirent(accepted);
    probeDataInventory(accepted);
    orbis::probeCtype();
    probeSavePaths(accepted);
    }

  out.ok   = true;
  out.root = accepted;
  out.argv.push_back("Gothic2Notr");
  out.argv.push_back("-g");
  out.argv.push_back(accepted);
  for(const std::string& a : cfg.args)
    out.argv.push_back(a);

  std::string joined;
  for(const std::string& a : out.argv) {
    joined += a;
    joined.push_back(' ');
    }
  ps4_log("boot: argv = %s",joined.c_str());
  return out;
  }

}

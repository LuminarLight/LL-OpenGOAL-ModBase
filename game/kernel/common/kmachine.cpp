#include "kmachine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION
// NOTE - this is needed, because on macOS, there is a file called `MacTypes.h`
// inside it, it defines something named `Ptr`
// Our `Ptr` is not namespaced, so there is ambiguity.
//
// Second fix is because miniaudio redefines functions in the stdlib based on bad pre-processor
// assumptions AppleClang apparently does not define POSIX macros, leading to future ambiguity
namespace MiniAudioLib {
#if defined(__APPLE__)
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#include "third-party/miniaudio.h"
#undef _POSIX_C_SOURCE
#else
// It should work if it's defined, but for some reason it didn't this is the unlikely branch
// but lets maintain the original value
#define NOT_REAL_OLD_POSIX_C_SOURCE _POSIX_C_SOURCE
#include "third-party/miniaudio.h"
#define _POSIX_C_SOURCE NOT_REAL_OLD_POSIX_C_SOURCE
#undef NOT_REAL_OLD_POSIX_C_SOURCE
#endif
#else
#include "third-party/miniaudio.h"
#endif
}  // namespace MiniAudioLib

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/font/font_utils.h"
#include "common/util/json_util.h"
#include "common/util/string_util.h"

#include "game/external/discord.h"
#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/screenshot.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/mips2c_table.h"
#include "game/runtime.h"
#include "game/sce/libcdvd_ee.h"
#include "game/sce/libpad.h"
#include "game/sce/libscf.h"
#include "game/sce/sif_ee.h"

/*!
 * Where does OVERLORD load its data from?
 */
OverlordDataSource isodrv;

// Get IOP modules from DVD or from dsefilesv
u32 modsrc;

// Reboot IOP with IOP kernel from DVD/CD on boot
u32 reboot_iop;

const char* init_types[] = {"fakeiso", "deviso", "iso_cd"};
u8 pad_dma_buf[2 * SCE_PAD_DMA_BUFFER_SIZE];

// added
u32 vif1_interrupt_handler = 0;
u32 vblank_interrupt_handler = 0;

Timer ee_clock_timer;

MiniAudioLib::ma_engine maEngine;

void kmachine_init_globals_common() {
  memset(pad_dma_buf, 0, sizeof(pad_dma_buf));
  isodrv = fakeiso;  // changed. fakeiso is the only one that works in opengoal.
  modsrc = 1;
  reboot_iop = 1;
  vif1_interrupt_handler = 0;
  vblank_interrupt_handler = 0;
  ee_clock_timer = Timer();
#ifdef _WIN32  // only do this on windows, because it only works on windows.
  MiniAudioLib::ma_engine_uninit(&maEngine);
#endif
  MiniAudioLib::ma_engine_init(NULL, &maEngine);
}

/*!
 * Initialize the CD Drive
 * DONE, EXACT
 */
void InitCD() {
  lg::info("Initializing CD drive. This may take a while...");
  ee::sceCdInit(SCECdINIT);
  ee::sceCdMmode(SCECdDVD);
  while (ee::sceCdDiskReady(0) == SCECdNotReady) {
    lg::debug("Drive not ready... insert a disk!");
  }
  lg::debug("Disk type {}\n", ee::sceCdGetDiskType());
}

/*!
 * Initialize the GS and display the splash screen.
 */
void InitVideo() {
  if (!SplashScreen) {
    lg::info("InitVideo: skipping splash!\n");
    return;
  }
  std::map<int, std::string> lang_to_splash_map{
      {SCE_JAPANESE_LANGUAGE, "JAP"},   {SCE_ENGLISH_LANGUAGE, "USA"},
      {SCE_FRENCH_LANGUAGE, "FRE"},     {SCE_SPANISH_LANGUAGE, "SPA"},
      {SCE_GERMAN_LANGUAGE, "GER"},     {SCE_ITALIAN_LANGUAGE, "ITA"},
      {SCE_PORTUGUESE_LANGUAGE, "POR"}, {SCE_KOREAN_LANGUAGE, "KOR"},
  };
  auto lang = ee::sceScfGetLanguage();
  if (!lang_to_splash_map.contains(lang)) {
    lg::warn("InitVideo: no splash for lang {}, falling back to english...\n", lang);
    lang = SCE_ENGLISH_LANGUAGE;
  }
  auto filename = "SCREEN1." + lang_to_splash_map.at(lang);
  auto path = file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] /
              "iso" / filename;
  if (lang != SCE_ENGLISH_LANGUAGE && !fs::exists(path)) {
    lg::warn("InitVideo: file {} not found, falling back to english...\n", filename);
    path = file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] / "iso" /
           "SCREEN1.USA";
  }
  if (!fs::exists(path)) {
    lg::warn("InitVideo: splash screen not found!\n");
    return;
  }
  auto data = file_util::read_binary_file(path);
  // width is always 512, height is sometimes different (e.g. demo screens), so we infer from file
  // size
  constexpr int kWidth = 512;
  if (data.size() % (kWidth * 4) != 0) {
    lg::error("InitVideo: splash size {} not divisible by stride {}", data.size(), kWidth * 4);
    return;
  }
  int kHeight = data.size() / (kWidth * 4);
  if ((int)data.size() != kWidth * kHeight * 4) {
    lg::error("InitVideo: unexpected size {}, expected {} for splash screen", data.size(),
              kWidth * kHeight * 4);
    return;
  }
  Gfx::g_splash.data = std::move(data);
  Gfx::g_splash.width = kWidth;
  Gfx::g_splash.height = kHeight;
  Gfx::g_splash.ready.store(true);
  SplashTimer.start();
  while (SplashTimer.getSeconds() < SPLASH_SCREEN_TIME) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

/*!
 * Flush caches.  Does all the memory, regardless of what you specify
 */
void CacheFlush(void* mem, int size) {
  (void)mem;
  (void)size;
  // FlushCache(0);
  // FlushCache(2);
}

/*!
 * Open a new controller pad.
 * Set the new_pad flag to 1 and state to 0.
 * Prints an error if it fails to open.
 */
u64 CPadOpen(u64 cpad_info, s32 pad_number) {
  auto cpad = Ptr<CPadInfo>(cpad_info).c();
  if (cpad->cpad_file == 0) {
    // not open, so we will open it
    cpad->cpad_file =
        ee::scePadPortOpen(pad_number, 0, pad_dma_buf + pad_number * SCE_PAD_DMA_BUFFER_SIZE);
    if (cpad->cpad_file < 1) {
      MsgErr("dkernel: !open cpad #%d (%d)\n", pad_number, cpad->cpad_file);
    }
    cpad->new_pad = 1;
    cpad->state = 0;
  }
  return cpad_info;
}

// custom music
// One set of time-synced layers plays at a time. GOAL starts and stops sets and pushes the
// volume, pause state and variant every frame. Loading runs on a separate thread and never
// calls into GOAL; everything shared is guarded by g_custom_music_mutex.
struct CustomMusicLayerDef {
  std::string id;
  std::string path;  // relative to the music dir
};

struct CustomMusicSetDef {
  int variant_fade_ms = 1500;
  bool stream = false;
  std::vector<CustomMusicLayerDef> layers;
  std::vector<std::vector<float>> variants;  // variants[v][i] = gain of layers[i] in variant v
};

struct CustomMusicLayer {
  std::string id;
  MiniAudioLib::ma_sound sound;
  bool inited = false;
};

struct CustomMusicSet {
  std::string name;
  CustomMusicSetDef def;
  std::vector<std::unique_ptr<CustomMusicLayer>> layers;
  MiniAudioLib::ma_sound_group group;
  bool group_inited = false;
  bool paused = false;
  int current_variant = -1;
  bool warned_variant = false;
  std::chrono::steady_clock::time_point last_sync_check;
  std::chrono::steady_clock::time_point fade_deadline;  // used when dying
};

static constexpr int kCustomMusicFadeMs = 1000;  // set start and stop fade

static std::mutex g_custom_music_mutex;
static std::unique_ptr<CustomMusicSet> g_custom_music_active;              // playing or about to
static std::vector<std::unique_ptr<CustomMusicSet>> g_custom_music_dying;  // fading out
static std::atomic<u64> g_custom_music_request{0};  // bumped on every start/stop
static std::atomic<int> g_custom_music_loaders{0};  // running loader threads
static std::atomic<bool> g_custom_music_shutdown{false};
static std::atomic<float> g_custom_music_volume{1.0f};  // last volume pushed from GOAL, 0-1
static std::atomic<int> g_custom_music_variant{0};      // last variant pushed from GOAL
static std::string g_custom_music_failed_name;          // do not retry this name
// engine time (pcm frames) when the last retired set is silent. A new set starts after it,
// so different music never crossfades.
static MiniAudioLib::ma_uint64 g_custom_music_silent_at = 0;

static fs::path custom_music_dir() {
  return file_util::get_jak_project_dir() / "custom_assets" / "jak1" / "custom_sound" / "music";
}

static std::string goal_string(u32 ptr) {
  return std::string(Ptr<String>(ptr).c()->data());
}

static float float_from_bits(u32 bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

static u32 goal_bool(bool b) {
  return b ? offset_of_s7() + jak1_symbols::FIX_SYM_TRUE : offset_of_s7();
}

static void set_custom_music_playing_flag(bool playing) {
  static auto sym = jak1::intern_from_c("*custom-music-playing?*");
  sym->value = goal_bool(playing);
}

// Layers first, group last. Safe to call on a half-loaded set.
static void custom_music_free_set(CustomMusicSet& set) {
  for (auto& layer : set.layers) {
    if (layer->inited) {
      MiniAudioLib::ma_sound_uninit(&layer->sound);
      layer->inited = false;
    }
  }
  if (set.group_inited) {
    MiniAudioLib::ma_sound_group_uninit(&set.group);
    set.group_inited = false;
  }
}

// Fade the active set out and move it to the dying list. Caller holds g_custom_music_mutex.
static void custom_music_retire_active_locked() {
  if (!g_custom_music_active) {
    return;
  }
  auto& set = *g_custom_music_active;
  MiniAudioLib::ma_sound_group_set_fade_in_milliseconds(
      &set.group, -1.0f, 0.0f, (MiniAudioLib::ma_uint64)kCustomMusicFadeMs);
  g_custom_music_silent_at = MiniAudioLib::ma_engine_get_time_in_pcm_frames(&maEngine) +
                             (MiniAudioLib::ma_uint64)kCustomMusicFadeMs *
                                 MiniAudioLib::ma_engine_get_sample_rate(&maEngine) / 1000;
  set.fade_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(kCustomMusicFadeMs + 100);
  g_custom_music_dying.push_back(std::move(g_custom_music_active));
}

// Only audio extensions, so other files with the same stem never match.
static fs::path custom_music_find_legacy_file(const std::string& name) {
  static const char* kExts[] = {".flac", ".mp3", ".wav", ".ogg"};
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(custom_music_dir(), ec)) {
    if (!entry.is_regular_file() || entry.path().stem().string() != name) {
      continue;
    }
    std::string ext = str_util::to_lower(entry.path().extension().string());
    for (auto* e : kExts) {
      if (ext == e) {
        return entry.path();
      }
    }
  }
  return {};
}

// Reads <name>.musicset.jsonc, or falls back to a single audio file with one variant.
static bool custom_music_load_set_def(const std::string& name, CustomMusicSetDef& out) {
  const fs::path set_file = custom_music_dir() / (name + ".musicset.jsonc");
  if (!fs::exists(set_file)) {
    fs::path legacy = custom_music_find_legacy_file(name);
    if (legacy.empty()) {
      lg::error("custom music: no set file or audio file for '{}'", name);
      return false;
    }
    out.layers.push_back({"base", legacy.filename().string()});
    out.variants.push_back({1.0f});
    return true;
  }

  try {
    auto j = parse_commented_json(file_util::read_text_file(set_file), set_file.string());
    out.variant_fade_ms = j.value("variant_fade_ms", 1500);
    out.stream = j.value("stream", false);

    // nlohmann sorts object keys, so layers are in alphabetical order. Gains are looked up by id.
    const auto& layers = j.at("layers");
    for (auto it = layers.begin(); it != layers.end(); ++it) {
      std::string rel = it.value().get<std::string>();
      if (fs::path(rel).is_absolute() || rel.find("..") != std::string::npos) {
        lg::error("custom music '{}': layer path '{}' must be relative and without ..", name, rel);
        return false;
      }
      if (!fs::exists(custom_music_dir() / rel)) {
        lg::error("custom music '{}': missing layer file '{}'", name, rel);
        return false;
      }
      out.layers.push_back({it.key(), rel});
    }
    if (out.layers.empty()) {
      lg::error("custom music '{}': no layers", name);
      return false;
    }

    const auto& variants = j.at("variants");
    if (!variants.is_array() || variants.empty()) {
      lg::error("custom music '{}': variants must be a non-empty array", name);
      return false;
    }
    for (const auto& v : variants) {
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (std::none_of(out.layers.begin(), out.layers.end(),
                         [&](const CustomMusicLayerDef& l) { return l.id == it.key(); })) {
          lg::warn("custom music '{}': variant uses unknown layer '{}'", name, it.key());
        }
      }
      std::vector<float> gains;
      for (const auto& layer : out.layers) {
        gains.push_back(v.value(layer.id, 0.0f));
      }
      out.variants.push_back(std::move(gains));
    }
  } catch (const std::exception& e) {
    lg::error("custom music '{}': cannot read set file: {}", name, e.what());
    return false;
  }
  return true;
}

static int custom_music_clamp_variant(const CustomMusicSetDef& def, int variant) {
  return (variant >= 0 && variant < (int)def.variants.size()) ? variant : 0;
}

static bool custom_music_load_layers(CustomMusicSet& set) {
  if (MiniAudioLib::ma_sound_group_init(&maEngine, 0, nullptr, &set.group) !=
      MiniAudioLib::MA_SUCCESS) {
    lg::error("custom music '{}': failed to create sound group", set.name);
    return false;
  }
  set.group_inited = true;
  const MiniAudioLib::ma_uint32 flags = set.def.stream ? MiniAudioLib::MA_SOUND_FLAG_STREAM : 0;
  for (const auto& ld : set.def.layers) {
    auto layer = std::make_unique<CustomMusicLayer>();
    layer->id = ld.id;
    fs::path full = custom_music_dir() / ld.path;
#ifdef _WIN32
    // Make music able to be found on Windows even if user folder has weird characters.
    std::wstring fullW = full.wstring();
    auto r = MiniAudioLib::ma_sound_init_from_file_w(&maEngine, fullW.c_str(), flags, &set.group,
                                                     nullptr, &layer->sound);
#else
    auto r = MiniAudioLib::ma_sound_init_from_file(&maEngine, full.string().c_str(), flags,
                                                   &set.group, nullptr, &layer->sound);
#endif
    if (r != MiniAudioLib::MA_SUCCESS) {
      lg::error("custom music '{}': failed to load layer '{}' ({})", set.name, ld.id,
                full.string());
      return false;
    }
    layer->inited = true;
    set.layers.push_back(std::move(layer));
  }

  if (set.layers.size() > 1) {
    MiniAudioLib::ma_uint64 len0 = 0;
    MiniAudioLib::ma_sound_get_length_in_pcm_frames(&set.layers[0]->sound, &len0);
    for (size_t i = 1; i < set.layers.size(); i++) {
      MiniAudioLib::ma_uint64 len = 0;
      MiniAudioLib::ma_sound_get_length_in_pcm_frames(&set.layers[i]->sound, &len);
      if ((len > len0 ? len - len0 : len0 - len) > 48) {
        lg::warn("custom music '{}': layer '{}' length differs from '{}', layers will drift",
                 set.name, set.layers[i]->id, set.layers[0]->id);
      }
    }
  }
  return true;
}

// One loader thread per request. It never touches GOAL, only the mutex-guarded globals.
static void custom_music_loader(std::string name, u64 my_request) {
  auto set = std::make_unique<CustomMusicSet>();
  set->name = name;
  bool ok = custom_music_load_set_def(name, set->def) && custom_music_load_layers(*set);

  std::lock_guard<std::mutex> lock(g_custom_music_mutex);
  const bool stale = g_custom_music_request.load() != my_request || g_custom_music_shutdown.load();
  if (!ok || stale) {
    custom_music_free_set(*set);
    if (!ok && !stale) {
      g_custom_music_failed_name = name;  // do not retry until the symbol changes
    }
    g_custom_music_loaders--;
    return;
  }

  // Start every layer on the same frame, a little in the future (50 ms), and not before the
  // previous set has faded out.
  MiniAudioLib::ma_uint64 start =
      std::max(MiniAudioLib::ma_engine_get_time_in_pcm_frames(&maEngine) +
                   MiniAudioLib::ma_engine_get_sample_rate(&maEngine) / 20,
               g_custom_music_silent_at);
  const int v0 = custom_music_clamp_variant(set->def, g_custom_music_variant.load());
  set->current_variant = v0;
  MiniAudioLib::ma_sound_group_set_volume(&set->group, g_custom_music_volume.load());
  MiniAudioLib::ma_sound_group_start(&set->group);
  for (size_t i = 0; i < set->layers.size(); i++) {
    auto& s = set->layers[i]->sound;
    MiniAudioLib::ma_sound_set_looping(&s, MA_TRUE);
    MiniAudioLib::ma_sound_set_start_time_in_pcm_frames(&s, start);
    MiniAudioLib::ma_sound_set_fade_in_milliseconds(&s, 0.0f, set->def.variants[v0][i],
                                                    (MiniAudioLib::ma_uint64)kCustomMusicFadeMs);
    MiniAudioLib::ma_sound_start(&s);
  }
  lg::info("custom music: playing '{}' ({} layers, variant {})", name, set->layers.size(), v0);
  g_custom_music_active = std::move(set);
  g_custom_music_loaders--;
}

// Crossfade every layer to its gain in the requested variant. No seek, no restart.
static void custom_music_apply_variant(CustomMusicSet& set, int variant) {
  const int v = custom_music_clamp_variant(set.def, variant);
  if (v != variant && !set.warned_variant) {
    set.warned_variant = true;
    lg::warn("custom music '{}': variant {} does not exist in the set file, using 0", set.name,
             variant);
  }
  if (v == set.current_variant) {
    return;
  }
  lg::info("custom music '{}': variant {} -> {}", set.name, set.current_variant, v);
  for (size_t i = 0; i < set.layers.size(); i++) {
    MiniAudioLib::ma_sound_set_fade_in_milliseconds(
        &set.layers[i]->sound, -1.0f, set.def.variants[v][i],
        (MiniAudioLib::ma_uint64)set.def.variant_fade_ms);
  }
  set.current_variant = v;
}

// Re-align layers that drifted from layer 0. Only for fully decoded layers.
static void custom_music_sync_layers(CustomMusicSet& set,
                                     std::chrono::steady_clock::time_point now) {
  if (set.def.stream || set.paused || set.layers.size() < 2 ||
      now - set.last_sync_check < std::chrono::milliseconds(250)) {
    return;
  }
  set.last_sync_check = now;
  auto& ref_sound = set.layers[0]->sound;
  MiniAudioLib::ma_uint64 ref = 0, ref_after = 0, len = 0;
  MiniAudioLib::ma_sound_get_length_in_pcm_frames(&ref_sound, &len);
  MiniAudioLib::ma_sound_get_cursor_in_pcm_frames(&ref_sound, &ref);
  std::vector<MiniAudioLib::ma_uint64> cursors(set.layers.size(), 0);
  for (size_t i = 1; i < set.layers.size(); i++) {
    MiniAudioLib::ma_sound_get_cursor_in_pcm_frames(&set.layers[i]->sound, &cursors[i]);
  }
  MiniAudioLib::ma_sound_get_cursor_in_pcm_frames(&ref_sound, &ref_after);
  if (ref_after != ref) {
    return;  // the mixer ran between the reads, try again next time
  }
  for (size_t i = 1; i < set.layers.size(); i++) {
    long long diff = (long long)cursors[i] - (long long)ref;
    if (len > 0) {
      if (diff > (long long)len / 2) {
        diff -= (long long)len;
      } else if (diff < -(long long)len / 2) {
        diff += (long long)len;
      }
    }
    if (std::llabs(diff) > 64) {
      lg::warn("custom music '{}': layer '{}' drifted {} frames, re-aligning", set.name,
               set.layers[i]->id, diff);
      MiniAudioLib::ma_sound_seek_to_pcm_frame(&set.layers[i]->sound, ref);
    }
  }
}

u32 play_custom_music(u32 file_name) {
  std::string name = goal_string(file_name);
  if (name.find('+') != std::string::npos) {
    lg::error("custom music: '+' is reserved for layer files, refusing '{}'", name);
    return offset_of_s7();
  }
  std::lock_guard<std::mutex> lock(g_custom_music_mutex);
  if (name == g_custom_music_failed_name) {
    return offset_of_s7();  // already failed once, the player waits for the symbol to change
  }
  g_custom_music_failed_name.clear();
  custom_music_retire_active_locked();
  u64 my_request = ++g_custom_music_request;
  g_custom_music_loaders++;
  set_custom_music_playing_flag(true);
  std::thread(custom_music_loader, name, my_request).detach();
  return offset_of_s7() + jak1_symbols::FIX_SYM_TRUE;
}

u32 stop_custom_music() {
  std::lock_guard<std::mutex> lock(g_custom_music_mutex);
  g_custom_music_request++;  // cancels a load that is still running
  custom_music_retire_active_locked();
  return offset_of_s7() + jak1_symbols::FIX_SYM_TRUE;
}

// Called once per frame from GOAL. volume_bits is a float (0-100) passed as raw bits.
u32 custom_music_update(u32 volume_bits, u32 paused, u32 variant) {
  g_custom_music_variant = (int)variant;
  const float vol = float_from_bits(volume_bits) / 100.0f;
  g_custom_music_volume = vol;
  const bool now_paused = paused != 0;
  const auto now = std::chrono::steady_clock::now();
  bool playing;
  {
    std::lock_guard<std::mutex> lock(g_custom_music_mutex);
    auto apply = [&](CustomMusicSet& set) {
      MiniAudioLib::ma_sound_group_set_volume(&set.group, vol);
      if (set.paused != now_paused) {
        // stopping the group freezes its layers, starting it resumes them in sync
        if (now_paused) {
          MiniAudioLib::ma_sound_group_stop(&set.group);
        } else {
          MiniAudioLib::ma_sound_group_start(&set.group);
        }
        set.paused = now_paused;
      }
    };
    if (g_custom_music_active) {
      auto& set = *g_custom_music_active;
      apply(set);
      custom_music_apply_variant(set, (int)variant);
      custom_music_sync_layers(set, now);
    }
    for (auto& dying : g_custom_music_dying) {
      apply(*dying);
    }
    // free sets whose fade-out is over
    for (auto it = g_custom_music_dying.begin(); it != g_custom_music_dying.end();) {
      if (now >= (*it)->fade_deadline) {
        custom_music_free_set(**it);
        it = g_custom_music_dying.erase(it);
      } else {
        ++it;
      }
    }
    playing = g_custom_music_active != nullptr || g_custom_music_loaders.load() > 0;
  }
  set_custom_music_playing_flag(playing);
  return goal_bool(playing);
}

void custom_music_shutdown() {
  g_custom_music_shutdown = true;
  {
    std::lock_guard<std::mutex> lock(g_custom_music_mutex);
    if (g_custom_music_active) {
      custom_music_free_set(*g_custom_music_active);
      g_custom_music_active = nullptr;
    }
    for (auto& d : g_custom_music_dying) {
      custom_music_free_set(*d);
    }
    g_custom_music_dying.clear();
  }
  // loader threads see g_custom_music_shutdown and free their own sets, wait briefly for them
  for (int i = 0; i < 200 && g_custom_music_loaders.load() > 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/*!
 * Not checked super carefully for jak 2, but looks the same
 */
u64 CPadGetData(u64 cpad_info) {
  using namespace ee;
  auto cpad = Ptr<CPadInfo>(cpad_info).c();
  auto pad_state = scePadGetState(cpad->number, 0);
  if (pad_state == scePadStateDiscon) {
    cpad->state = 0;
  }
  cpad->valid = pad_state | 0x80;
  switch (cpad->state) {
    // case 99: // functional
    default:  // controller is functioning as normal
      if (pad_state == scePadStateStable || pad_state == scePadStateFindCTP1) {
        scePadRead(cpad->number, 0, (u8*)cpad);
        // ps2 controllers would send an enabled bit if the button was NOT pressed, but we don't do
        // that here. removed code that flipped the bits.

        if (cpad->change_time) {
          scePadSetActDirect(cpad->number, 0, cpad->direct);
        }
        cpad->valid = pad_state;
      }
      break;
    case 0:  // unavailable
      if (pad_state == scePadStateStable || pad_state == scePadStateFindCTP1) {
        auto pad_mode = scePadInfoMode(cpad->number, 0, InfoModeCurID, 0);
        if (pad_mode != 0) {
          auto vibration_mode = scePadInfoMode(cpad->number, 0, InfoModeCurExID, 0);
          if (vibration_mode > 0) {
            // vibration supported
            pad_mode = vibration_mode;
          }
          if (pad_mode == 4) {
            // controller mode
            cpad->state = 40;
          } else if (pad_mode == 7) {
            // dualshock mode
            cpad->state = 70;
          } else {
            // who knows mode
            cpad->state = 90;
          }
        }
      }
      break;
    case 40:  // controller mode - check for extra modes
      // cpad->change_time = 0;
      cpad->change_time = 0;
      if (scePadInfoMode(cpad->number, 0, InfoModeIdTable, -1) == 0) {
        // no controller modes
        cpad->state = 90;
        return cpad_info;
      }
      cpad->state = 41;
    case 41:  // controller mode - change to dualshock mode!
      // try to enter the 2nd controller mode (dualshock for ds2's)
      if (scePadSetMainMode(cpad->number, 0, 1, 3) == 1) {
        cpad->state = 42;
      }
      break;
    case 42:  // controller mode change check
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateFailed) {
        // failed to change to DS2
        cpad->state = 41;
      }
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateComplete) {
        // change successful. go back to the beginning.
        cpad->state = 0;
      }
      break;
    case 70:  // dualshock mode - check vibration
      // get number of actuators (2 for DS2)
      if (scePadInfoAct(cpad->number, 0, -1, 0) < 1) {
        // no actuators means no vibration. skip to end!
        // cpad->change_time = 0;
        cpad->change_time = 0;
        cpad->state = 99;
      } else {
        // we have actuators to use.
        // cpad->change_time = 1;  // remember to update pad times.
        cpad->change_time = 1;
        cpad->state = 75;
      }
      break;
    case 75:  // set actuator vib param info
      if (scePadSetActAlign(cpad->number, 0, cpad->align) != 0) {
        if (scePadInfoPressMode(cpad->number, 0) == 1) {
          // pressure buttons supported
          cpad->state = 76;
        } else {
          // no pressure buttons, done with controller setup
          cpad->state = 99;
        }
      }
      break;
    case 76:  // enter pressure mode
      if (scePadEnterPressMode(cpad->number, 0) == 1) {
        cpad->state = 78;
      }
      break;
    case 78:  // pressure mode request check
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateFailed) {
        cpad->state = 76;
      }
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateComplete) {
        cpad->state = 99;
      }
      break;
    case 90:
      break;  // unsupported controller. too bad!
  }
  return cpad_info;
}

// should make sure this works the same way in jak 2
void InstallHandler(u32 handler_idx, u32 handler_func) {
  switch (handler_idx) {
    case 3:
      vblank_interrupt_handler = handler_func;
      break;
    case 5:
      vif1_interrupt_handler = handler_func;
      break;
    default:
      lg::error("unknown handler: {}\n", handler_idx);
      ASSERT(false);
  }
}

// nothing used this in jak1, hopefully same for 2
void InstallDebugHandler() {
  ASSERT(false);
}

/*!
 * Get length of a file.
 */
s32 klength(u64 fs) {
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    // first flag bit not set. This means no errors
    auto end_seek = ee::sceLseek(file_stream->file, 0, SCE_SEEK_END);
    auto reset_seek = ee::sceLseek(file_stream->file, 0, SCE_SEEK_SET);
    if (reset_seek < 0 || end_seek < 0) {
      // seeking failed, flag it
      file_stream->flags |= 1;
    }
    return end_seek;
  } else {
    return 0;
  }
}

/*!
 * Seek a file stream.
 */
s32 kseek(u64 fs, s32 offset, s32 where) {
  s32 result = -1;
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    result = ee::sceLseek(file_stream->file, offset, where);
    if (result < 0) {
      file_stream->flags |= 1;
    }
  }
  return result;
}

/*!
 * Read from a file stream.
 */
s32 kread(u64 fs, u64 buffer, s32 size) {
  s32 result = -1;
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    result = ee::sceRead(file_stream->file, Ptr<u8>(buffer).c(), size);
    if (result < 0) {
      file_stream->flags |= 1;
    }
  }
  return result;
}

/*!
 * Write to a file stream.
 */
s32 kwrite(u64 fs, u64 buffer, s32 size) {
  s32 result = -1;
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    result = ee::sceWrite(file_stream->file, Ptr<u8>(buffer).c(), size);
    if (result < 0) {
      file_stream->flags |= 1;
    }
  }
  return result;
}

s32 kmkdir(u64 name) {
  char acStack_90[128];
  if (Ptr<String>(name)->data()[4] == '/') {  // start from the fourth character?
    sprintf(acStack_90, "%s", Ptr<String>(name)->data() + 5);
  } else {
    sprintf(acStack_90, "host:%s", Ptr<String>(name)->data() + 4);
  }
  return ee::sceMkDir(acStack_90, 0x1fd);
}

/*!
 * Close a file stream.
 */
u64 kclose(u64 fs) {
  auto file_stream = Ptr<FileStream>(fs).c();
  if ((file_stream->flags ^ 1) & 1) {
    ee::sceClose(file_stream->file);
    file_stream->file = -1;
  }
  file_stream->flags = 0;
  return fs;
}

// TODO dma_to_iop
void dma_to_iop() {
  ASSERT(false);
}

u64 DecodeLanguage() {
  return masterConfig.language;
}

u64 DecodeAspect() {
  return masterConfig.aspect;
}

u64 DecodeVolume() {
  return masterConfig.volume;
}

// NOTE: this is originally hardcoded, and returns different values depending on the disc region.
// it returns 0 for NTSC-U, 1 for PAL and 2 for NTSC-J
u64 DecodeTerritory() {
  return GAME_TERRITORY_SCEA;
}

u64 DecodeTimeout() {
  return masterConfig.timeout;
}

u64 DecodeInactiveTimeout() {
  return masterConfig.inactive_timeout;
}

void DecodeTime(u32 ptr) {
  Ptr<ee::sceCdCLOCK> clock(ptr);
  // in jak2, if this fails, they do a sceScfGetLocalTimefromRTC
  sceCdReadClock(clock.c());
}

void vif_interrupt_callback(int bucket_id) {
  // added for the PC port for faking VIF interrupts from the graphics system.
  if (vif1_interrupt_handler && MasterExit == RuntimeExitStatus::RUNNING) {
    call_goal(Ptr<Function>(vif1_interrupt_handler), bucket_id, 0, 0, s7.offset, g_ee_main_mem);
  }
}

/// PC PORT FUNCTIONS BEGIN

u32 offset_of_s7() {
  return s7.offset;
}

inline bool symbol_to_bool(const u32 symptr) {
  return symptr != s7.offset;
}

inline u64 bool_to_symbol(const bool val) {
  return val ? static_cast<u64>(s7.offset) + true_symbol_offset(g_game_version) : s7.offset;
}

u64 pc_filter_debug_string(u32 str_ptr, u32 dist_ptr) {
  auto str = std::string(Ptr<String>(str_ptr).c()->data());
  float dist;
  memcpy(&dist, &dist_ptr, 4);

  // Check distance first
  if (Gfx::g_debug_settings.text_check_range) {
    if (dist / 4096.F > Gfx::g_debug_settings.text_max_range) {
      return bool_to_symbol(true);
    }
  }

  // Get the current filters
  const auto& filters = Gfx::g_debug_settings.text_filters;
  if (filters.empty()) {
    // there are no filters, exit early
    return bool_to_symbol(false);
  }

  // Currently very dumb contains check
  for (const auto& filter : filters) {
    if (filter.type == DebugTextFilter::Type::CONTAINS) {
      if (!str.empty() && !filter.content.empty() && !str_util::contains(str, filter.content)) {
        return bool_to_symbol(true);
      }
    } else if (filter.type == DebugTextFilter::Type::NOT_CONTAINS) {
      if (!str.empty() && !filter.content.empty() && str_util::contains(str, filter.content)) {
        return bool_to_symbol(true);
      }
    } else if (filter.type == DebugTextFilter::Type::REGEX) {
      if (str_util::valid_regex(filter.content) &&
          std::regex_match(str, std::regex(filter.content))) {
        return bool_to_symbol(true);
      }
    }
  }
  return bool_to_symbol(false);
}

CommonPCPortFunctionWrappers g_pc_port_funcs;

u64 read_ee_timer() {
  u64 ns = ee_clock_timer.getNs();
  return (ns * 3) / 10;
}

void pc_memmove(u32 dst, u32 src, u32 size) {
  memmove(Ptr<u8>(dst).c(), Ptr<u8>(src).c(), size);
}

void send_gfx_dma_chain(u32 /*bank*/, u32 chain) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->send_chain(g_ee_main_mem, chain);
  }
}

void pc_texture_upload_now(u32 page, u32 mode) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->texture_upload_now(Ptr<u8>(page).c(), mode, s7.offset);
  }
}

void pc_force_reload_all() {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->force_reload_all();
  }
}

void pc_force_reload_level(u32 name) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->force_reload_level(std::string(Ptr<String>(name).c()->data()));
  }
}

void pc_force_reload_common() {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->force_reload_common();
  }
}

void pc_texture_relocate(u32 dst, u32 src, u32 format) {
  if (Gfx::GetCurrentRenderer()) {
    Gfx::GetCurrentRenderer()->texture_relocate(dst, src, format);
  }
}

u64 pc_get_mips2c(u32 name) {
  const char* n = Ptr<String>(name).c()->data();
  return Mips2C::gLinkedFunctionTable.get(n);
}

u64 pc_get_display_id() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->get_active_display_index();
  }
  return 0;
}

void pc_set_display_id(u64 display_id) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_display_id(display_id);
  }
}

u64 pc_get_display_name(u32 id, u32 str_dest_ptr) {
  std::string name = "";
  if (Display::GetMainDisplay()) {
    name = Display::GetMainDisplay()->get_display_manager()->get_connected_display_name(id);
  }
  if (name.empty()) {
    return bool_to_symbol(false);
  }
  if (g_game_version == GameVersion::Jak1) {
    // The Jak 1 font has only caps
    name = str_util::to_upper(name).c_str();
  }
  // Encode the string to the game font
  const auto encoded_name = get_font_bank_from_game_version(g_game_version)
                                ->convert_utf8_to_game(str_util::titlize(name).c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), encoded_name.c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::titlize(encoded_name).c_str());
  return bool_to_symbol(true);
}

u32 pc_get_display_mode() {
  auto display_mode = game_settings::DisplaySettings::DisplayMode::Windowed;
  if (Display::GetMainDisplay()) {
    display_mode = Display::GetMainDisplay()->get_display_manager()->get_display_mode();
  }
  switch (display_mode) {
    case game_settings::DisplaySettings::DisplayMode::Borderless:
      return g_pc_port_funcs.intern_from_c("borderless").offset;
    case game_settings::DisplaySettings::DisplayMode::Fullscreen:
      return g_pc_port_funcs.intern_from_c("fullscreen").offset;
    default:
      return g_pc_port_funcs.intern_from_c("windowed").offset;
  }
}

void pc_set_display_mode(u32 symptr, u64 window_width, u64 window_height) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (symptr == g_pc_port_funcs.intern_from_c("windowed").offset || symptr == s7.offset) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_display_mode(
        game_settings::DisplaySettings::DisplayMode::Windowed, window_width, window_height);
  } else if (symptr == g_pc_port_funcs.intern_from_c("borderless").offset) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_display_mode(
        game_settings::DisplaySettings::DisplayMode::Borderless, window_width, window_height);
  } else if (symptr == g_pc_port_funcs.intern_from_c("fullscreen").offset) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_display_mode(
        game_settings::DisplaySettings::DisplayMode::Fullscreen, window_width, window_height);
  }
}

u64 pc_get_display_count() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->num_connected_displays();
  }
  return 0;
}

void pc_get_active_display_size(u32 w_ptr, u32 h_ptr) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (w_ptr) {
    auto w_out = Ptr<s64>(w_ptr).c();
    if (w_out) {
      *w_out = Display::GetMainDisplay()->get_display_manager()->get_screen_width();
    }
  }
  if (h_ptr) {
    auto h_out = Ptr<s64>(h_ptr).c();
    if (h_out) {
      *h_out = Display::GetMainDisplay()->get_display_manager()->get_screen_height();
    }
  }
}

s64 pc_get_active_display_refresh_rate() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->get_active_display_refresh_rate();
  }
  return 0;
}

void pc_get_window_size(u32 w_ptr, u32 h_ptr) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (w_ptr) {
    auto w = Ptr<s64>(w_ptr).c();
    if (w) {
      *w = Display::GetMainDisplay()->get_display_manager()->get_window_width();
    }
  }
  if (h_ptr) {
    auto h = Ptr<s64>(h_ptr).c();
    if (h) {
      *h = Display::GetMainDisplay()->get_display_manager()->get_window_height();
    }
  }
}

void pc_get_window_scale(u32 x_ptr, u32 y_ptr) {
  if (!Display::GetMainDisplay()) {
    return;
  }
  if (x_ptr) {
    auto x = Ptr<float>(x_ptr).c();
    if (x) {
      *x = Display::GetMainDisplay()->get_display_manager()->get_window_scale_x();
    }
  }
  if (y_ptr) {
    auto y = Ptr<float>(y_ptr).c();
    if (y) {
      *y = Display::GetMainDisplay()->get_display_manager()->get_window_scale_y();
    }
  }
}

void pc_set_window_size(u64 width, u64 height) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_display_manager()->enqueue_set_window_size(width, height);
  }
}

s64 pc_get_num_resolutions(u32 for_windowed) {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_display_manager()->get_num_resolutions(
        symbol_to_bool(for_windowed));
  }
  return 0;
}

void pc_get_resolution(u32 id, u32 for_windowed, u32 w_ptr, u32 h_ptr) {
  if (Display::GetMainDisplay()) {
    auto res = Display::GetMainDisplay()->get_display_manager()->get_resolution(
        id, symbol_to_bool(for_windowed));
    auto w = Ptr<s64>(w_ptr).c();
    if (w) {
      *w = res.width;
    }
    auto h = Ptr<s64>(h_ptr).c();
    if (h) {
      *h = res.height;
    }
  }
}

u64 pc_is_supported_resolution(u64 width, u64 height) {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_display_manager()->is_supported_resolution(width, height));
  }
  return bool_to_symbol(false);
}

u64 pc_get_controller_name(u32 id, u32 str_dest_ptr) {
  std::string name = "";
  if (Display::GetMainDisplay()) {
    name = Display::GetMainDisplay()->get_input_manager()->get_controller_name(id);
  }
  if (name.empty()) {
    return bool_to_symbol(false);
  }

  if (g_game_version == GameVersion::Jak1) {
    // The Jak 1 font has only caps
    name = str_util::to_upper(name).c_str();
  }
  // Encode the string to the game font
  const auto encoded_name = get_font_bank_from_game_version(g_game_version)
                                ->convert_utf8_to_game(str_util::titlize(name).c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), encoded_name.c_str());
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::titlize(encoded_name).c_str());
  return bool_to_symbol(true);
}

u64 pc_get_current_bind(s32 bind_assignment_info, u32 str_dest_ptr) {
  if (!Display::GetMainDisplay()) {
    // TODO - return something that lets the runtime use a translatable string if unknown
    strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::to_upper("unknown").c_str());
    return bool_to_symbol(true);
  }

  auto info = bind_assignment_info ? Ptr<BindAssignmentInfo>(bind_assignment_info).c() : NULL;
  auto port = (int)info->port;
  auto device_type = (int)info->device_type;
  auto for_button = info->buttons != s7.offset;
  auto input_idx = (int)info->input_idx;
  auto analog_min_range = info->analog_min_range != s7.offset;

  if (Display::GetMainDisplay()) {
    auto name = Display::GetMainDisplay()->get_input_manager()->get_current_bind(
        port, (InputDeviceType)device_type, for_button, input_idx, analog_min_range);
    if (name.empty()) {
      return bool_to_symbol(false);
    }
    if (g_game_version == GameVersion::Jak1) {
      // The Jak 1 font has only caps
      name = str_util::to_upper(name).c_str();
    }
    // Encode the string to the game font
    const auto encoded_name = get_font_bank_from_game_version(g_game_version)
                                  ->convert_utf8_to_game(str_util::titlize(name).c_str());
    strcpy(Ptr<String>(str_dest_ptr).c()->data(), encoded_name.c_str());
    return bool_to_symbol(true);
  }
  // TODO - return something that lets the runtime use a translatable string if unknown
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), str_util::to_upper("unknown").c_str());
  return bool_to_symbol(true);
}

u64 pc_get_controller_count() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_input_manager()->get_num_controllers();
  }
  return 0;
}

u64 pc_get_controller_index(u32 port) {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_input_manager()->get_controller_index(port);
  }
  return 0;
}

void pc_set_controller(u32 controller_id, u32 port) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_controller_for_port(controller_id, port);
  }
}

u32 pc_get_keyboard_enabled() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->is_keyboard_enabled());
  }
  return bool_to_symbol(false);
}

void pc_set_keyboard_enabled(u32 sym_val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enable_keyboard(symbol_to_bool(sym_val));
  }
}

void pc_set_mouse_options(u32 enabled, u32 control_camera, u32 control_movement) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_update_mouse_options(
        symbol_to_bool(enabled), symbol_to_bool(control_camera), symbol_to_bool(control_movement));
  }
}

void pc_set_mouse_camera_sens(u32 xsens, u32 ysens) {
  float xsens_val;
  memcpy(&xsens_val, &xsens, 4);
  float ysens_val;
  memcpy(&ysens_val, &ysens, 4);
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_camera_sens(xsens_val, ysens_val);
  }
}

void pc_ignore_background_controller_events(u32 sym_val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_ignore_background_controller_events(
        symbol_to_bool(sym_val));
  }
}

u64 pc_current_controller_has_led() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->controller_has_led(0));
  }
  return bool_to_symbol(false);
}

u64 pc_current_controller_has_rumble() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->controller_has_rumble(0));
  }
  return bool_to_symbol(false);
}

void pc_set_controller_led(const int port, const u8 red, const u8 green, const u8 blue) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_set_controller_led(port, red, green,
                                                                               blue);
  }
}

u64 pc_waiting_for_bind() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(Display::GetMainDisplay()->get_input_manager()->get_waiting_for_bind());
  }
  return bool_to_symbol(false);
}

void pc_set_waiting_for_bind(InputDeviceType device_type,
                             u32 for_analog,
                             u32 for_minimum_analog,
                             s32 input_idx) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_wait_for_bind(
        device_type, symbol_to_bool(for_analog), symbol_to_bool(for_minimum_analog), input_idx);
  }
}

void pc_stop_waiting_for_bind() {
  if (Display::GetMainDisplay()) {
    return Display::GetMainDisplay()->get_input_manager()->stop_waiting_for_bind();
  }
}

void pc_reset_bindings_to_defaults(const int port, const InputDeviceType device_type) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->reset_input_bindings_to_defaults(port,
                                                                                     device_type);
  }
}

void pc_set_auto_hide_cursor(u32 val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_set_auto_hide_mouse(
        symbol_to_bool(val));
  }
}

u64 pc_get_pressure_sensitivity_enabled() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->is_pressure_sensitivity_enabled());
  }
  return bool_to_symbol(false);
}

void pc_set_pressure_sensitivity_enabled(u32 val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->set_pressure_sensitivity_enabled(
        symbol_to_bool(val));
  }
}

void pc_set_axis_scale(u32 val) {
  if (Display::GetMainDisplay()) {
    // wow dangerous i guess
    Display::GetMainDisplay()->get_input_manager()->set_axis_scale(*((float*)&val));
  }
}

u32 pc_get_axis_scale() {
  float out = 1.33f;
  if (Display::GetMainDisplay()) {
    out = Display::GetMainDisplay()->get_input_manager()->axis_scale();
  }
  // wow dangerous i guess
  return *((u32*)&out);
}

u64 pc_current_controller_has_pressure_sensitivity() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->controller_has_pressure_sensitivity_support(
            0));
  }
  return bool_to_symbol(false);
}

u64 pc_current_controller_has_trigger_effect_support() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->controller_has_trigger_effect_support(0));
  }
  return bool_to_symbol(false);
}

u64 pc_get_trigger_effects_enabled() {
  if (Display::GetMainDisplay()) {
    return bool_to_symbol(
        Display::GetMainDisplay()->get_input_manager()->are_trigger_effects_enabled());
  }
  return bool_to_symbol(false);
}

void pc_set_trigger_effects_enabled(u32 val) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_set_trigger_effects_enabled(
        symbol_to_bool(val));
  }
}

void pc_clear_trigger_effect(dualsense_effects::TriggerEffectOption option) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_clear_trigger_effect(0,
                                                                                            option);
  }
}

void pc_send_trigger_effect_feedback(dualsense_effects::TriggerEffectOption option,
                                     u8 position,
                                     u8 strength) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_effect_feedback(
        0, option, position, strength);
  }
}

void pc_send_trigger_effect_vibrate(dualsense_effects::TriggerEffectOption option,
                                    u8 position,
                                    u8 amplitude,
                                    u8 frequency) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_effect_vibrate(
        0, option, position, amplitude, frequency);
  }
}

void pc_send_trigger_effect_weapon(dualsense_effects::TriggerEffectOption option,
                                   u8 start_position,
                                   u8 end_position,
                                   u8 strength) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_effect_weapon(
        0, option, start_position, end_position, strength);
  }
}

void pc_send_trigger_rumble(u16 left_rumble, u16 right_rumble, u32 duration_ms) {
  if (Display::GetMainDisplay()) {
    Display::GetMainDisplay()->get_input_manager()->enqueue_controller_send_trigger_rumble(
        0, left_rumble, right_rumble, duration_ms);
  }
}

void pc_set_vsync(u32 sym_val) {
  Gfx::g_global_settings.vsync = symbol_to_bool(sym_val);
}

void pc_set_msaa(int samples) {
  Gfx::g_global_settings.msaa_samples = samples;
}

void pc_set_frame_rate(int rate) {
  Gfx::g_global_settings.target_fps = rate;
}

void pc_set_game_resolution(int w, int h) {
  Gfx::g_global_settings.game_res_w = w;
  Gfx::g_global_settings.game_res_h = h;
}

void pc_set_letterbox(int w, int h) {
  Gfx::g_global_settings.lbox_w = w;
  Gfx::g_global_settings.lbox_h = h;
}

void pc_set_brightness_contrast(s32 color, s32 alpha) {
  Gfx::g_global_settings.brightness_contrast_color = color;
  Gfx::g_global_settings.brightness_contrast_alpha = alpha;
}

void pc_renderer_tree_set_lod(Gfx::RendererTreeType tree, int lod) {
  switch (tree) {
    case Gfx::RendererTreeType::TFRAG3:
      Gfx::g_global_settings.lod_tfrag = lod;
      break;
    case Gfx::RendererTreeType::TIE3:
      Gfx::g_global_settings.lod_tie = lod;
      break;
    default:
      lg::error("Invalid tree {} specified for SetLod ({})", fmt::underlying(tree), lod);
      break;
  }
}

void pc_set_collision_mask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask, u32 symptr) {
  if (symbol_to_bool(symptr)) {
    Gfx::CollisionRendererSetMask(mode, mask);
  } else {
    Gfx::CollisionRendererClearMask(mode, mask);
  }
}

u32 pc_get_collision_mask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask) {
  return Gfx::CollisionRendererGetMask(mode, mask) ? s7.offset + true_symbol_offset(g_game_version)
                                                   : s7.offset;
}

void pc_set_collision_wireframe(u32 symptr) {
  Gfx::g_global_settings.collision_wireframe = symbol_to_bool(symptr);
}

void pc_set_collision(u32 symptr) {
  Gfx::g_global_settings.collision_enable = symbol_to_bool(symptr);
}

void pc_set_gfx_hack(u64 which, u32 symptr) {
  switch (which) {
    case 0:  // no tex
      Gfx::g_global_settings.hack_no_tex = symbol_to_bool(symptr);
      break;
  }
}

u32 pc_get_os() {
#ifdef _WIN32
  return g_pc_port_funcs.intern_from_c("windows").offset;
#elif __linux__
  return g_pc_port_funcs.intern_from_c("linux").offset;
#elif __APPLE__
  return g_pc_port_funcs.intern_from_c("darwin").offset;
#else
  return s7.offset;
#endif
}

time_t pc_get_unix_timestamp() {
  return std::time(nullptr);
}

u64 pc_filepath_exists(u32 filepath) {
  auto filepath_str = std::string(Ptr<String>(filepath).c()->data());
  return bool_to_symbol(fs::exists(filepath_str));
}

u64 pc_mkdir_filepath(u32 filepath) {
  auto filepath_str = std::string(Ptr<String>(filepath).c()->data());
  return bool_to_symbol(file_util::create_dir_if_needed_for_file(filepath_str));
}

void pc_prof(u32 name, ProfNode::Kind kind) {
  prof().event(Ptr<String>(name).c()->data(), kind);
}

std::mt19937 extra_random_generator;
u32 pc_rand() {
  return (u32)extra_random_generator();
}

void pc_treat_pad0_as_pad1(u32 symptr) {
  Gfx::g_debug_settings.treat_pad0_as_pad1 = symbol_to_bool(symptr);
}

u32 pc_is_imgui_visible() {
  return bool_to_symbol(Gfx::g_debug_settings.show_imgui);
}

void pc_screen_shot() {
  g_want_screenshot = true;
}

void pc_register_screen_shot_settings(u32 ptr) {
  register_screen_shot_settings(Ptr<ScreenShotSettings>(ptr).c());
}

void pc_encode_utf8_string(u32 src_str_ptr, u32 str_dest_ptr) {
  auto str = std::string(Ptr<String>(src_str_ptr).c()->data());
  std::string version = version_to_game_name(g_game_version);
  const std::string font_bank_name = version == "jak1" ? "jak1-v2" : version;
  std::string converted =
      get_font_bank(get_text_version_from_name(font_bank_name))->convert_utf8_to_game(str);
  strcpy(Ptr<String>(str_dest_ptr).c()->data(), converted.c_str());
}

/// Initializes all functions that are common across all game versions
/// These functions have the same implementation and do not use any game specific functions (other
/// than the one to create a function in the first place)
void init_common_pc_port_functions(
    std::function<Ptr<Function>(const char*, void*)> make_func_symbol_func,
    std::function<InternFromCInfo(const char*)> intern_from_c_func,
    std::function<u64(const char*)> make_string_from_c_func) {
  g_pc_port_funcs.intern_from_c = intern_from_c_func;
  g_pc_port_funcs.make_string_from_c = make_string_from_c_func;
  // Get a 300MHz timer value. Called from EE thread
  make_func_symbol_func("__read-ee-timer", (void*)read_ee_timer);
  // Do a fast memory copy.
  make_func_symbol_func("__mem-move", (void*)pc_memmove);
  // Called from game thread to submit rendering DMA chain.
  make_func_symbol_func("__send-gfx-dma-chain", (void*)send_gfx_dma_chain);
  // Called from game thread to upload a texture outside of the main DMA chain.
  make_func_symbol_func("__pc-texture-upload-now", (void*)pc_texture_upload_now);
  make_func_symbol_func("__pc-texture-relocate", (void*)pc_texture_relocate);
  // Called from the game thread at initialization. The game thread is the only one to touch the
  // mips2c function table (through the linker and ugh this function), so no locking is needed.
  make_func_symbol_func("__pc-get-mips2c", (void*)pc_get_mips2c);
  make_func_symbol_func("__pc-force-reload-all-levels", (void*)pc_force_reload_all);
  make_func_symbol_func("__pc-force-reload-level", (void*)pc_force_reload_level);
  make_func_symbol_func("__pc-force-reload-common-level", (void*)pc_force_reload_common);

  // -- DISPLAY RELATED --
  // Returns the name of the display with the given id or #f if not found / empty
  make_func_symbol_func("pc-get-display-id", (void*)pc_get_display_id);
  make_func_symbol_func("pc-set-display-id!", (void*)pc_set_display_id);
  make_func_symbol_func("pc-get-display-name", (void*)pc_get_display_name);
  make_func_symbol_func("pc-get-display-mode", (void*)pc_get_display_mode);
  make_func_symbol_func("pc-set-display-mode!", (void*)pc_set_display_mode);
  make_func_symbol_func("pc-get-display-count", (void*)pc_get_display_count);
  // Returns resolution of the monitor's current display mode
  make_func_symbol_func("pc-get-active-display-size", (void*)pc_get_active_display_size);
  // Returns the current refresh rate of the currently selected monitor's display mode.
  make_func_symbol_func("pc-get-active-display-refresh-rate",
                        (void*)pc_get_active_display_refresh_rate);
  // Returns size of window. Called from game thread
  make_func_symbol_func("pc-get-window-size", (void*)pc_get_window_size);
  // Returns scale of window. This is for DPI stuff.
  make_func_symbol_func("pc-get-window-scale", (void*)pc_get_window_scale);
  make_func_symbol_func("pc-set-window-size!", (void*)pc_set_window_size);
  make_func_symbol_func("pc-get-num-resolutions", (void*)pc_get_num_resolutions);
  make_func_symbol_func("pc-get-resolution", (void*)pc_get_resolution);
  make_func_symbol_func("pc-is-supported-resolution?", (void*)pc_is_supported_resolution);

  // -- INPUT RELATED --
  // Returns the name of the display with the given id or #f if not found / empty
  make_func_symbol_func("pc-get-controller-name", (void*)pc_get_controller_name);
  make_func_symbol_func("pc-get-current-bind", (void*)pc_get_current_bind);
  make_func_symbol_func("pc-get-controller-count", (void*)pc_get_controller_count);
  make_func_symbol_func("pc-get-controller-index", (void*)pc_get_controller_index);
  make_func_symbol_func("pc-set-controller!", (void*)pc_set_controller);
  make_func_symbol_func("pc-get-keyboard-enabled?", (void*)pc_get_keyboard_enabled);
  make_func_symbol_func("pc-set-keyboard-enabled!", (void*)pc_set_keyboard_enabled);
  make_func_symbol_func("pc-set-mouse-options!", (void*)pc_set_mouse_options);
  make_func_symbol_func("pc-set-mouse-camera-sens!", (void*)pc_set_mouse_camera_sens);
  make_func_symbol_func("pc-ignore-background-controller-events!",
                        (void*)pc_ignore_background_controller_events);
  make_func_symbol_func("pc-current-controller-has-led?", (void*)pc_current_controller_has_led);
  make_func_symbol_func("pc-current-controller-has-rumble?",
                        (void*)pc_current_controller_has_rumble);
  make_func_symbol_func("pc-set-controller-led!", (void*)pc_set_controller_led);
  make_func_symbol_func("pc-waiting-for-bind?", (void*)pc_waiting_for_bind);
  make_func_symbol_func("pc-set-waiting-for-bind!", (void*)pc_set_waiting_for_bind);
  make_func_symbol_func("pc-stop-waiting-for-bind!", (void*)pc_stop_waiting_for_bind);
  make_func_symbol_func("pc-reset-bindings-to-defaults!", (void*)pc_reset_bindings_to_defaults);
  make_func_symbol_func("pc-set-auto-hide-cursor!", (void*)pc_set_auto_hide_cursor);
  make_func_symbol_func("pc-get-pressure-sensitivity-enabled?",
                        (void*)pc_get_pressure_sensitivity_enabled);
  make_func_symbol_func("pc-set-pressure-sensitivity-enabled!",
                        (void*)pc_set_pressure_sensitivity_enabled);
  make_func_symbol_func("pc-set-axis-scale!", (void*)pc_set_axis_scale);
  make_func_symbol_func("pc-get-axis-scale", (void*)pc_get_axis_scale);
  make_func_symbol_func("pc-current-controller-has-pressure-sensitivity?",
                        (void*)pc_current_controller_has_pressure_sensitivity);
  make_func_symbol_func("pc-current-controller-has-trigger-effect-support?",
                        (void*)pc_current_controller_has_trigger_effect_support);
  make_func_symbol_func("pc-get-trigger-effects-enabled?", (void*)pc_get_trigger_effects_enabled);
  make_func_symbol_func("pc-set-trigger-effects-enabled!", (void*)pc_set_trigger_effects_enabled);
  make_func_symbol_func("pc-clear-trigger-effect!", (void*)pc_clear_trigger_effect);
  make_func_symbol_func("pc-send-trigger-effect-feedback!", (void*)pc_send_trigger_effect_feedback);
  make_func_symbol_func("pc-send-trigger-effect-vibrate!", (void*)pc_send_trigger_effect_vibrate);
  make_func_symbol_func("pc-send-trigger-effect-weapon!", (void*)pc_send_trigger_effect_weapon);
  make_func_symbol_func("pc-send-trigger-rumble!", (void*)pc_send_trigger_rumble);

  // graphics things
  make_func_symbol_func("pc-set-vsync", (void*)pc_set_vsync);
  make_func_symbol_func("pc-set-msaa", (void*)pc_set_msaa);
  make_func_symbol_func("pc-set-frame-rate", (void*)pc_set_frame_rate);
  make_func_symbol_func("pc-set-game-resolution", (void*)pc_set_game_resolution);
  make_func_symbol_func("pc-set-brightness-contrast", (void*)pc_set_brightness_contrast);
  make_func_symbol_func("pc-set-letterbox", (void*)pc_set_letterbox);
  make_func_symbol_func("pc-renderer-tree-set-lod", (void*)pc_renderer_tree_set_lod);
  make_func_symbol_func("pc-set-collision-mode", (void*)Gfx::CollisionRendererSetMode);
  make_func_symbol_func("pc-set-collision-mask", (void*)pc_set_collision_mask);
  make_func_symbol_func("pc-get-collision-mask", (void*)pc_get_collision_mask);
  make_func_symbol_func("pc-set-collision-wireframe", (void*)pc_set_collision_wireframe);
  make_func_symbol_func("pc-set-collision", (void*)pc_set_collision);
  make_func_symbol_func("pc-set-gfx-hack", (void*)pc_set_gfx_hack);

  // -- OTHER --
  // Return the current OS as a symbol. Actually returns what it was compiled for!
  make_func_symbol_func("pc-get-os", (void*)pc_get_os);
  make_func_symbol_func("pc-get-unix-timestamp", (void*)pc_get_unix_timestamp);
  make_func_symbol_func("pc-treat-pad0-as-pad1", (void*)pc_treat_pad0_as_pad1);
  make_func_symbol_func("pc-is-imgui-visible?", (void*)pc_is_imgui_visible);

  // file related functions
  make_func_symbol_func("pc-filepath-exists?", (void*)pc_filepath_exists);
  make_func_symbol_func("pc-mkdir-file-path", (void*)pc_mkdir_filepath);

  // custom music
  make_func_symbol_func("play-custom-music", (void*)play_custom_music);
  make_func_symbol_func("stop-custom-music", (void*)stop_custom_music);
  make_func_symbol_func("custom-music-update", (void*)custom_music_update);

  // discord rich presence
  make_func_symbol_func("pc-discord-rpc-set", (void*)set_discord_rpc);

  // profiler
  make_func_symbol_func("pc-prof", (void*)pc_prof);

  // RNG
  make_func_symbol_func("pc-rand", (void*)pc_rand);

  // text
  make_func_symbol_func("pc-encode-utf8-string", (void*)pc_encode_utf8_string);

  // debugging tools
  make_func_symbol_func("pc-filter-debug-string?", (void*)pc_filter_debug_string);
  make_func_symbol_func("pc-screen-shot", (void*)pc_screen_shot);
  make_func_symbol_func("pc-register-screen-shot-settings",
                        (void*)pc_register_screen_shot_settings);
}

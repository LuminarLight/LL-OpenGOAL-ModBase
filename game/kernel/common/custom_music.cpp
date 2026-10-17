#include "custom_music.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// must come before miniaudio: it pulls in the system headers that miniaudio would otherwise
// include inside namespace MiniAudioLib
#include "game/kernel/common/kmachine.h"

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

#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"
#include "common/util/string_util.h"
#include "common/versions/versions.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

static MiniAudioLib::ma_engine maEngine;
static bool g_ma_engine_inited = false;

void custom_music_init_engine() {
#ifdef _WIN32  // only do this on windows, because it only works on windows.
  if (g_ma_engine_inited) {
    MiniAudioLib::ma_engine_uninit(&maEngine);
    g_ma_engine_inited = false;
  }
#endif
  g_ma_engine_inited = MiniAudioLib::ma_engine_init(NULL, &maEngine) == MiniAudioLib::MA_SUCCESS;
  if (!g_ma_engine_inited) {
    lg::error("custom music: audio engine failed to start, custom music is disabled");
  }
}

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
  std::chrono::steady_clock::time_point fade_deadline;
};

static constexpr int kCustomMusicFadeMs = 1000;

static std::mutex g_custom_music_mutex;
static std::unique_ptr<CustomMusicSet> g_custom_music_active;
static std::vector<std::unique_ptr<CustomMusicSet>> g_custom_music_dying;
static std::atomic<u64> g_custom_music_request{0};  // bumped on every start/stop
static std::atomic<int> g_custom_music_loaders{0};
static std::atomic<bool> g_custom_music_shutdown{false};
static std::atomic<float> g_custom_music_volume{1.0f};
static std::atomic<int> g_custom_music_variant{0};
static std::string g_custom_music_failed_name;
// engine time (pcm frames) when the last retired set is silent. A new set starts after it,
// so different music never crossfades.
static MiniAudioLib::ma_uint64 g_custom_music_silent_at = 0;

static fs::path custom_music_dir() {
  return file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] /
         "custom_sound" / "music";
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

// Caller holds g_custom_music_mutex.
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

static void custom_music_loader(std::string name, u64 my_request) {
  auto set = std::make_unique<CustomMusicSet>();
  set->name = name;
  bool ok = custom_music_load_set_def(name, set->def) && custom_music_load_layers(*set);

  std::lock_guard<std::mutex> lock(g_custom_music_mutex);
  const bool stale = g_custom_music_request.load() != my_request || g_custom_music_shutdown.load();
  if (!ok || stale) {
    custom_music_free_set(*set);
    if (!ok && !stale) {
      g_custom_music_failed_name = name;
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
  if (!g_ma_engine_inited) {
    return offset_of_s7();
  }
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
  for (int i = 0; i < 200 && g_custom_music_loaders.load() > 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

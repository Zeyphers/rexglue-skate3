#include <rex/audio/custom_music.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>

namespace rex::audio::custom_music {
namespace {

constexpr uint32_t kSamplesPerFrame = 512;
constexpr size_t kKeyBytes = 64;

struct Entry {
  uint64_t key;
  uint32_t slot;
  int32_t position;
};

struct State {
  std::filesystem::path root;
  std::mutex mutex;
  bool active = false;
  uint32_t song_count = 0;
  std::vector<Entry> entries;
  std::vector<std::unique_ptr<memory::MappedMemory>> slots;
};

State& state() {
  static State s;
  return s;
}

// FNV-1a over packet bytes 0..2 and 4..63; byte 3 (skip count) is left out in case the
// game's streamer rewrites it. Must match packet_key() in tools/build_music_mod.py.
uint64_t PacketKey(const uint8_t* packet) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < kKeyBytes; ++i) {
    if (i == 3) {
      continue;
    }
    h ^= packet[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

void Load(State& s) {
  std::lock_guard lock(s.mutex);
  if (s.active) {
    return;
  }
  if (s.root.empty()) {
    REXLOG_WARN("custom music: SetGameRoot has not been called yet");
    return;
  }
  const auto dir = s.root / "custom_music";
  const auto table_path = dir / "table.bin";
  std::ifstream in(table_path, std::ios::binary);
  if (!in) {
    REXLOG_WARN("custom music: could not open {}", table_path.string());
    return;
  }
  char magic[4];
  uint32_t version = 0, count = 0;
  in.read(magic, 4);
  in.read(reinterpret_cast<char*>(&version), 4);
  in.read(reinterpret_cast<char*>(&count), 4);
  if (!in || std::memcmp(magic, "S3CM", 4) != 0 || version != 1) {
    REXLOG_ERROR("custom music: {} is not a valid table", table_path.string());
    REXAPU_ERROR("custom music: {} is not a valid table", table_path.string());
    return;
  }
  constexpr uint32_t kSlots = 46;
  uint32_t slot_samples[kSlots];
  in.read(reinterpret_cast<char*>(slot_samples), sizeof(slot_samples));
  s.entries.resize(count);
  for (auto& e : s.entries) {
    in.read(reinterpret_cast<char*>(&e.key), 8);
    in.read(reinterpret_cast<char*>(&e.slot), 4);
    in.read(reinterpret_cast<char*>(&e.position), 4);
  }
  if (!in) {
    REXLOG_ERROR("custom music: table.bin is truncated");
    REXAPU_ERROR("custom music: table.bin is truncated");
    s.entries.clear();
    return;
  }
  s.slots.resize(kSlots);
  uint32_t mapped = 0;
  for (uint32_t i = 0; i < kSlots; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "slot%02u.pcm", i);
    const auto path = dir / name;
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) == 0 || ec) {
      continue;
    }
    s.slots[i] = memory::MappedMemory::Open(path, memory::MappedMemory::Mode::kRead);
    if (s.slots[i]) {
      ++mapped;
    }
  }
  // slots.txt lists "NN  Artist - Title" per slot; short playlists repeat songs to fill
  // all 46 slots, so count the distinct names.
  std::set<std::string> songs;
  std::ifstream slots_txt(dir / "slots.txt");
  for (std::string line; std::getline(slots_txt, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    size_t name = line.find_first_not_of(' ');
    name = line.find_first_not_of("0123456789", name);
    name = line.find_first_not_of(' ', name);
    if (name != std::string::npos) {
      songs.insert(line.substr(name));
    }
  }
  s.song_count = songs.empty() ? mapped : uint32_t(songs.size());
  s.active = true;
  REXLOG_INFO("custom music: activated {} tagged packets, {} songs from {}", count, mapped, dir.string());
  REXAPU_INFO("custom music: activated {} tagged packets, {} songs from {}", count, mapped, dir.string());
}

}  // namespace

void SetGameRoot(const std::filesystem::path& root) {
  auto& s = state();
  s.root = root;
  Load(s);
}

bool LookupPacket(const uint8_t* packet, uint32_t* slot, int32_t* position) {
  auto& s = state();
  if (!s.active) {
    if (s.root.empty()) {
      return false;
    }
    Load(s);
    if (!s.active) {
      return false;
    }
  }
  if (!packet) {
    return false;
  }
  const uint64_t key = PacketKey(packet);
  auto it = std::lower_bound(s.entries.begin(), s.entries.end(), key,
                             [](const Entry& e, uint64_t k) { return e.key < k; });
  if (it == s.entries.end() || it->key != key) {
    return false;
  }
  *slot = it->slot;
  *position = it->position;

  static uint32_t s_matches = 0;
  if (++s_matches <= 10 || (s_matches % 1000) == 0) {
    REXLOG_INFO("custom music: match #{} -> slot {}, position {}", s_matches, *slot, *position);
  }
  return true;
}

void RenderFrame(uint32_t slot, int32_t position, bool stereo, uint8_t* out) {
  auto& s = state();
  const size_t out_bytes = size_t(kSamplesPerFrame) * 2 * (stereo ? 2 : 1);
  std::memset(out, 0, out_bytes);
  if (slot >= s.slots.size() || !s.slots[slot]) {
    return;
  }
  const uint8_t* pcm = s.slots[slot]->data();
  const int64_t total = int64_t(s.slots[slot]->size() / 4);  // stereo s16 frames
  for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
    const int64_t p = int64_t(position) + i;
    if (p < 0 || p >= total) {
      continue;
    }
    const uint8_t* src = pcm + p * 4;
    if (stereo) {
      std::memcpy(out + i * 4, src, 4);
    } else {
      const int16_t l = int16_t((src[0] << 8) | src[1]);
      const int16_t r = int16_t((src[2] << 8) | src[3]);
      const int16_t m = int16_t((int32_t(l) + r) / 2);
      out[i * 2] = uint8_t(uint16_t(m) >> 8);
      out[i * 2 + 1] = uint8_t(m);
    }
  }
}

uint32_t SongCount() {
  auto& s = state();
  if (!s.active && !s.root.empty()) {
    Load(s);
  }
  return s.active ? s.song_count : 0;
}

}  // namespace rex::audio::custom_music

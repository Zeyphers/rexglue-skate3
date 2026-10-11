#pragma once

// Custom playlist music for Skate 3.
//
// tools/build_music_mod.py rebuilds the game's playlist stream out of EA-XMA packets that
// each carry a unique tag, and writes <game>/custom_music/table.bin mapping every tagged
// packet to (playlist slot, sample position) plus one PCM file per slot. When the XMA
// decoder meets one of those packets it outputs the slot's PCM instead of decoding.

#include <cstdint>
#include <filesystem>

namespace rex::audio::custom_music {

// Called once the game data root is known; the mod is loaded lazily from
// <root>/custom_music on first lookup.
void SetGameRoot(const std::filesystem::path& root);

// If `packet` (a 2 KiB XMA packet in guest memory) is one of the tagged packets,
// returns true with its slot and the sample position of the first frame that begins in it.
bool LookupPacket(const uint8_t* packet, uint32_t* slot, int32_t* position);

// Writes one XMA frame (512 samples) of the slot's audio starting at `position`
// to `out` as big-endian s16, interleaved when `stereo`.
void RenderFrame(uint32_t slot, int32_t position, bool stereo, uint8_t* out);

// Number of different songs in the installed custom playlist (0 when the mod is not installed).
uint32_t SongCount();

}  // namespace rex::audio::custom_music

#pragma once

#include <cstdint>
#include <string>

struct NdsSpuSaveState;

// Nintendo DS ARM7 sound unit. Register widths are bytes (1/2/4), matching
// nds_io_read/write. The mixer advances once per 1024 system cycles.
void nds_spu_reset();
// Console power-off silences queued host output but preserves architectural
// channel/register state, matching melonDS SPU::Stop().
void nds_spu_stop();
uint32_t nds_spu_read(uint32_t addr, uint32_t width);
void nds_spu_write(uint32_t addr, uint32_t value, uint32_t width);
void nds_tick_spu(uint64_t system_cycles);

// Real-time audio catch-up. While emulation runs below real time, the
// frontend sets how many output frames the host device is short of
// (nds_spu_set_catchup_owed, replacing the previous amount). A title hook in
// the guest sound driver's tick loop then calls nds_spu_catchup with one
// driver tick's worth of system cycles: when that much is still owed, it
// mixes it at once, without moving the hardware timeline, and returns true
// so the hook runs one more driver tick. Host-paced, so guest-visible sound
// timing follows the host clock while the game is slow.
void nds_spu_set_catchup_owed(uint32_t frames);
bool nds_spu_catchup(uint32_t system_cycles);
// Output frames mixed by nds_spu_catchup so far (monotonic).
uint64_t nds_spu_catchup_frames();

// Stereo signed-16 output retained for the eventual SDL host. Returns frames.
uint32_t nds_spu_read_output(int16_t* stereo, uint32_t frames);

// Always-on, non-destructive sample trace for native/oracle comparison. Sample
// ordinals start at zero after reset and are independent of the SDL reader.
// A bounded history is retained; callers must request start >= oldest.
uint64_t nds_spu_debug_output_produced();
uint64_t nds_spu_debug_output_oldest();
uint32_t nds_spu_debug_copy_output(uint64_t start, int16_t* stereo,
                                   uint32_t frames);

bool spu_savestate_export(NdsSpuSaveState* out);
bool spu_savestate_validate(const NdsSpuSaveState& in, std::string* error);
bool spu_savestate_import(const NdsSpuSaveState& in, std::string* error);

// Increments whenever restored guest state invalidates already-buffered host
// presentation audio. Frontends clear their backend queue on an epoch change.
uint64_t nds_spu_presentation_epoch();

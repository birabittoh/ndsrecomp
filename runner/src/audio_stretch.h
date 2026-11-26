#pragma once

// Host audio queue between the emulation thread (producer) and the audio
// device thread (consumer). The consumer plays it through a WSOLA time
// stretcher, so a game running below full speed slows the audio down at its
// normal pitch instead of running the device dry (gaps and crackle). While
// the queue holds its usual level the output is the input sample for sample.
//
// Grains are W frames, laid every H = W/2 output frames with a periodic Hann
// window (two overlapping windows sum to 1). Each grain starts near a nominal
// input position that advances H * tempo per grain; within +-kSeek of it, the
// grain that best continues the previous one (cross-correlation) is taken.
// The natural continuation matches itself perfectly, so it is kept whenever
// it lies in the seek window: at tempo 1 nothing is ever searched.
//
// Tempo comes from the smoothed queue level: 1 at or above kLowFrames,
// falling linearly to kMinTempo at kW. With nothing new arriving, the
// stretcher keeps repeating matched grains of what it has for kDroneFrames,
// then fades to silence until the producer catches up again.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

class NdsAudioStretchQueue {
public:
    static constexpr uint32_t kCapacity = 65536;  // frames, power of two
    static constexpr uint32_t kW = 1024;          // grain (~31 ms)
    static constexpr uint32_t kH = kW / 2;        // output hop
    static constexpr uint32_t kSeek = 256;        // +- seek window (~8 ms)
    static constexpr uint32_t kLowFrames = kW + 256;
    static constexpr double kMinTempo = 0.25;
    static constexpr uint32_t kDroneFrames = 4096;   // ~125 ms
    static constexpr float kFadeOutStep = 1.0f / 1600.0f;
    static constexpr float kFadeInStep = 1.0f / 320.0f;

    NdsAudioStretchQueue()
        : ring_(size_t(kCapacity) * 2u), window_(kW) {
        constexpr double kPi = 3.14159265358979323846;
        for (uint32_t i = 0; i < kW; ++i)
            window_[i] = static_cast<float>(
                0.5 - 0.5 * std::cos(2.0 * kPi * i / kW));
        reset_locked();
    }

    // Producer. False when there is no room for the whole block.
    bool push(const int16_t* stereo, uint32_t frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (kCapacity - static_cast<uint32_t>(write_ - trim_) < frames)
            return false;
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t at = static_cast<uint32_t>(write_ + i) & (kCapacity - 1);
            ring_[at * 2u] = stereo[i * 2u];
            ring_[at * 2u + 1u] = stereo[i * 2u + 1u];
        }
        write_ += frames;
        return true;
    }

    // Frames still to be played: queued input past the nominal position plus
    // rendered output not yet handed to the device.
    uint32_t queued() {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t pos = static_cast<uint64_t>(pos_);
        return static_cast<uint32_t>(write_ > pos ? write_ - pos : 0) +
               out_count_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_locked();
    }

    // Grains played below tempo 1 so far (monotonic): the queue ran low.
    uint64_t slowed_hops() {
        std::lock_guard<std::mutex> lock(mutex_);
        return slowed_hops_;
    }

    // Consumer. Fills `frames` stereo frames; returns how many of them were
    // forced silence (starved), 0 while playing.
    uint32_t render(int16_t* out, uint32_t frames, bool* gap_started) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t silent = 0;
        *gap_started = false;
        for (uint32_t done = 0; done < frames;) {
            if (out_count_ == 0 && !hop(gap_started)) {
                std::memset(out + done * 2u, 0,
                            size_t(frames - done) * 2u * sizeof(int16_t));
                silent += frames - done;
                break;
            }
            const uint32_t take = std::min(frames - done, out_count_);
            const float* src = out_ + (kH - out_count_) * 2u;
            for (uint32_t i = 0; i < take; ++i) {
                gain_ = fade_target_ > gain_
                    ? std::min(fade_target_, gain_ + kFadeInStep)
                    : std::max(fade_target_, gain_ - kFadeOutStep);
                for (uint32_t c = 0; c < 2; ++c) {
                    const float v = src[i * 2u + c] * gain_;
                    out[(done + i) * 2u + c] = static_cast<int16_t>(
                        std::lrintf(std::clamp(v, -32768.0f, 32767.0f)));
                }
            }
            out_count_ -= take;
            done += take;
        }
        return silent;
    }

private:
    float sample(uint64_t frame, uint32_t channel) const {
        return ring_[(static_cast<uint32_t>(frame) & (kCapacity - 1)) * 2u +
                     channel];
    }
    float mono(uint64_t frame) const {
        return sample(frame, 0) + sample(frame, 1);
    }

    void reset_locked() {
        trim_ = write_;
        pos_ = static_cast<double>(write_);
        prev_end_ = write_;
        last_write_ = write_;
        have_prev_ = false;
        smoothed_ = 0.0;
        out_count_ = 0;
        std::fill(acc_, acc_ + kH * 2u, 0.0f);
        stalled_ = 0;
        gain_ = 1.0f;
        fade_target_ = 1.0f;
        playing_ = false;
    }

    // Best grain start in [lo, hi] to continue the previous grain: coarse
    // pass on every other frame and lag, then a fine pass around the winner.
    uint64_t seek(uint64_t lo, uint64_t hi) const {
        auto score = [&](uint64_t s, uint32_t step) {
            double xy = 0.0, yy = 0.0;
            for (uint32_t i = 0; i < kH; i += step) {
                const double x = mono(prev_end_ + i);
                const double y = mono(s + i);
                xy += x * y;
                yy += y * y;
            }
            return xy / std::sqrt(yy + 1.0);
        };
        uint64_t best = lo;
        double best_score = -1e300;
        for (uint64_t s = lo; s <= hi; s += 2) {
            const double v = score(s, 2);
            if (v > best_score) { best_score = v; best = s; }
        }
        const uint64_t flo = best > lo ? best - 1 : lo;
        const uint64_t fhi = std::min(hi, best + 1);
        best_score = -1e300;
        for (uint64_t s = flo; s <= fhi; ++s) {
            const double v = score(s, 1);
            if (v > best_score) { best_score = v; best = s; }
        }
        return best;
    }

    // Renders the next kH output frames into out_. False when there is not
    // even one grain to play (only right after a reset).
    bool hop(bool* gap_started) {
        const uint64_t avail_end = write_;
        if (!have_prev_ && avail_end - prev_end_ < kW) return false;

        // Tempo from the smoothed level (~100 ms time constant).
        const uint64_t ipos = static_cast<uint64_t>(pos_);
        const double level = avail_end > ipos ? double(avail_end - ipos) : 0.0;
        smoothed_ = have_prev_ ? smoothed_ + (level - smoothed_) * 0.15 : level;
        double tempo = 1.0;
        if (smoothed_ < kLowFrames) {
            const double t = (smoothed_ - kW) / double(kLowFrames - kW);
            tempo = kMinTempo + (1.0 - kMinTempo) * std::clamp(t, 0.0, 1.0);
        }

        uint64_t s = prev_end_;
        if (have_prev_) {
            const uint64_t lo = std::max(trim_, ipos > kSeek ? ipos - kSeek : 0);
            const uint64_t hi = std::min(ipos + kSeek, avail_end - kW);
            if (hi < lo) {
                s = std::max(trim_, std::min(lo, avail_end - kW));
            } else if (prev_end_ < lo || prev_end_ > hi) {
                s = seek(lo, hi);
            }
        }

        // No new input reaching the grain: keep going on matched repeats for
        // a while, then fade out; fade back in once input flows again.
        const bool starving = avail_end == last_write_;
        last_write_ = avail_end;
        if (starving) {
            stalled_ += kH;
            if (stalled_ > kDroneFrames && fade_target_ > 0.0f) {
                fade_target_ = 0.0f;
                if (playing_) *gap_started = true;
            }
        } else {
            stalled_ = 0;
            fade_target_ = 1.0f;
        }

        for (uint32_t i = 0; i < kH; ++i) {
            for (uint32_t c = 0; c < 2; ++c) {
                out_[i * 2u + c] = acc_[i * 2u + c] +
                    window_[i] * sample(s + i, c);
                acc_[i * 2u + c] = window_[kH + i] * sample(s + kH + i, c);
            }
        }
        if (tempo < 1.0) ++slowed_hops_;
        out_count_ = kH;
        prev_end_ = s + kH;
        have_prev_ = true;
        playing_ = true;
        // At tempo 1 the nominal position follows the played grains again,
        // so drift left by a slow stretch cannot force a later seek.
        pos_ = tempo < 1.0 ? pos_ + kH * tempo : double(prev_end_);
        const double max_pos = double(avail_end - kW);
        if (pos_ > max_pos) pos_ = max_pos;
        if (pos_ < double(trim_)) pos_ = double(trim_);
        const uint64_t npos = static_cast<uint64_t>(pos_);
        const uint64_t keep = std::min(prev_end_,
                                       npos > kSeek ? npos - kSeek : 0);
        trim_ = std::max(trim_, keep);
        return true;
    }

    uint64_t slowed_hops_ = 0;
    std::mutex mutex_;
    std::vector<int16_t> ring_;
    std::vector<float> window_;
    uint64_t write_ = 0;     // absolute frame index, producer end
    uint64_t trim_ = 0;      // oldest frame the stretcher may still read
    double pos_ = 0.0;       // nominal input position
    uint64_t prev_end_ = 0;  // natural continuation of the last grain
    uint64_t last_write_ = 0;  // write_ seen by the previous grain
    bool have_prev_ = false;
    bool playing_ = false;
    double smoothed_ = 0.0;
    float acc_[kH * 2u]{};
    float out_[kH * 2u]{};
    uint32_t out_count_ = 0;
    uint32_t stalled_ = 0;
    float gain_ = 1.0f;
    float fade_target_ = 1.0f;
};

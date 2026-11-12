#include "audio_stream.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxAdjust = 0.005;   // +-0.5 %: below what the ear notices as pitch
constexpr double kFadeSeconds = 0.004;

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 32; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}
}  // namespace

float soft_limit(float v) {
    const float knee = 0.75f, room = 1.0f - knee;
    float a = std::fabs(v);
    if (a > knee) a = knee + room * std::tanh((a - knee) / room);
    return v < 0 ? -a : a;
}

void Limiter::set_rate(int rate) { release_ = float(1.0 - std::exp(-1.0 / (0.2 * rate))); }

void Limiter::process(float* frame, int n) {
    float peak = 0;
    for (int c = 0; c < n; c++) peak = std::max(peak, std::fabs(frame[c]));
    if (peak * gain_ > kLimit) gain_ = kLimit / peak;
    else gain_ += (1.0f - gain_) * release_;
    for (int c = 0; c < n; c++) frame[c] *= gain_;
}

AudioStream::AudioStream() {
    ring_.assign(kCapacity * kInChannels, 0.0f);
    set_output_rate(48000);
}

void AudioStream::set_output_rate(int rate) {
    if (rate == out_rate_) return;
    out_rate_ = rate;
    ramp_ = float(1.0 / (kFadeSeconds * rate));
    limiter_.set_rate(rate);
    // Headphones: the crossfeed is what the far ear hears, later and duller
    // (head shadow); the rear pair loses some treble, as sound from behind
    a_cross_ = float(1.0 - std::exp(-2 * kPi * 700.0 / rate));
    a_rear_ = float(1.0 - std::exp(-2 * kPi * 4000.0 / rate));
    dlen_ = std::clamp(int(0.0003 * rate + 0.5), 1, kDelay - 1);
    // Low-pass below the lower of the two Nyquist frequencies (with some room
    // for the transition band of a 16-tap filter), Kaiser window
    const double cutoff = std::min(1.0, double(rate) / kInRate) * 0.91, beta = 7.0, half = kTaps / 2;
    table_.assign(size_t(kPhases + 1) * kTaps, 0.0f);
    for (int p = 0; p <= kPhases; p++) {
        double t = (half - 1) + double(p) / kPhases, sum = 0, w[kTaps];
        for (int k = 0; k < kTaps; k++) {
            double x = k - t, r = x / half;
            double sinc = x == 0 ? 1.0 : std::sin(kPi * cutoff * x) / (kPi * cutoff * x);
            w[k] = std::fabs(r) >= 1 ? 0.0 : sinc * bessel_i0(beta * std::sqrt(1 - r * r)) / bessel_i0(beta);
            sum += w[k];
        }
        for (int k = 0; k < kTaps; k++) table_[size_t(p) * kTaps + k] = float(w[k] / sum);   // unity gain at DC
    }
}

size_t AudioStream::queued() const {
    return (write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire)) & (kCapacity - 1);
}

void AudioStream::push(const float* frames, size_t count) {
    size_t w = write_.load(std::memory_order_relaxed);
    size_t r = read_.load(std::memory_order_acquire);
    size_t room = kCapacity - 1 - ((w - r) & (kCapacity - 1));
    count = std::min(count, room);
    for (size_t i = 0; i < count; i++) {
        std::copy_n(frames + i * kInChannels, kInChannels, &ring_[w * kInChannels]);
        w = (w + 1) & (kCapacity - 1);
    }
    write_.store(w, std::memory_order_release);
}

// One input frame into the filter history: the next queued one, or the last
// one again while the queue is dry (the gain is fading out meanwhile)
void AudioStream::next_input() {
    size_t fill = queued();
    if (starved_ && fill >= kRefill) starved_ = false;
    if (!starved_ && fill == 0) {
        starved_ = true;
        underruns_++;
    }
    const float* last = hist_[hpos_];
    hpos_ = (hpos_ + 1) % kTaps;
    float* dst = hist_[hpos_];
    if (starved_) {
        std::copy_n(last, kInChannels, dst);
    } else {
        size_t r = read_.load(std::memory_order_relaxed);
        std::copy_n(&ring_[r * kInChannels], kInChannels, dst);
        read_.store((r + 1) & (kCapacity - 1), std::memory_order_release);
    }
    std::copy_n(dst, kInChannels, hist_[hpos_ + kTaps]);
}

void AudioStream::headphones(const float* in, float* out) {
    // Delay line: the crossfeed of each channel reaches the other ear ~0.3 ms later
    std::copy_n(in, kInChannels, delay_[dpos_]);
    const float* late = delay_[(dpos_ + kDelay - dlen_) % kDelay];
    dpos_ = (dpos_ + 1) % kDelay;
    float x[kInChannels];
    for (int c = 0; c < kInChannels; c++) x[c] = cross_lp_[c] += (late[c] - cross_lp_[c]) * a_cross_;
    float rear[2];
    for (int c = 0; c < 2; c++) {
        rear_lp_[c] += (in[2 + c] - rear_lp_[c]) * a_rear_;
        rear[c] = 0.6f * in[2 + c] + 0.4f * rear_lp_[c];
    }
    // Front: light crossfeed (0.3); rear: wider (0.5). Scaled so a centred
    // sound keeps about the level of the stereo mix
    out[0] = 0.5f * ((in[0] + 0.3f * x[1]) / 1.15f + (rear[0] + 0.5f * x[3]) / 1.25f);
    out[1] = 0.5f * ((in[1] + 0.3f * x[0]) / 1.15f + (rear[1] + 0.5f * x[2]) / 1.25f);
}

void AudioStream::pull(float* out, size_t count, Speakers mode) {
    // Rate control: a slow average of the queue level (the emulation pushes a
    // whole frame at a time) steers the resampling ratio
    double fill = double(queued());
    avg_fill_ += (fill - avg_fill_) * std::min(1.0, double(count) / out_rate_);
    double err = (avg_fill_ - double(kTarget)) / double(kTarget), dt = double(count) / out_rate_;
    drift_ = std::clamp(drift_ + err * kMaxAdjust * dt / 10.0, -kMaxAdjust, kMaxAdjust);   // the steady clock difference
    adjust_ = std::clamp(err * kMaxAdjust + drift_, -kMaxAdjust, kMaxAdjust);
    if (fill > double(kMaxFill)) skipping_ = true;
    const double step = double(kInRate) / out_rate_ * (1.0 + adjust_);

    for (size_t i = 0; i < count; i++) {
        while (frac_ >= 1.0) {
            next_input();
            frac_ -= 1.0;
        }
        double pf = frac_ * kPhases;
        int p = std::min(int(pf), kPhases - 1);
        float mu = float(pf - p);
        const float* w0 = &table_[size_t(p) * kTaps];
        const float* w1 = w0 + kTaps;
        float acc[kInChannels] = {};
        for (int k = 0; k < kTaps; k++) {
            float wk = w0[k] + (w1[k] - w0[k]) * mu;
            const float* h = hist_[hpos_ + 1 + k];
            for (int c = 0; c < kInChannels; c++) acc[c] += wk * h[c];
        }

        float target = starved_ || skipping_ ? 0.0f : 1.0f;
        gain_ = gain_ < target ? std::min(target, gain_ + ramp_) : std::max(target, gain_ - ramp_);
        if (skipping_ && gain_ == 0.0f) {   // faded out: drop the excess, then fade back in
            size_t q = queued();
            if (q > kTarget) read_.store((read_.load() + (q - kTarget)) & (kCapacity - 1), std::memory_order_release);
            avg_fill_ = double(kTarget);
            skipping_ = false;
            skips_++;
        }
        float frame[4];
        int n = channels(mode);
        switch (mode) {
        case Speakers::Stereo:
            frame[0] = (acc[0] + acc[2]) * 0.5f;
            frame[1] = (acc[1] + acc[3]) * 0.5f;
            break;
        case Speakers::Surround:   // the same power as the stereo mix when both pairs play the same
            for (int c = 0; c < 4; c++) frame[c] = acc[c] * 0.7071f;
            break;
        case Speakers::Headphones: headphones(acc, frame); break;
        }
        limiter_.process(frame, n);
        for (int c = 0; c < n; c++) out[i * size_t(n) + c] = soft_limit(frame[c] * gain_);
        frac_ += step;
    }
}

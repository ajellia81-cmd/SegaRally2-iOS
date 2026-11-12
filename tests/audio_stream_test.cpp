// The audio queue (audio_stream.cpp): a game clock and a device clock that
// disagree, a pause and a burst, simulated on one thread. The output has to
// stay a clean sine (no clicks), at the right level, with the queue bounded.
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio_stream.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kGameHz = 57.524160;
constexpr double kAmp = 0.5, kTone = 1000.0;

struct Result {
    double max_step = 0;     // largest jump between consecutive output samples
    double rms = 0;          // over the steady part
    size_t min_fill = ~size_t(0), max_fill = 0;
};

// Runs `seconds` of simulated time. The device runs `drift` faster than its
// nominal rate; the game is silent (pushes nothing) in [gap_from, gap_to) and
// pushes `burst` extra frames at `burst_at`.
Result run(AudioStream& s, int rate, double drift, double seconds, double gap_from = -1, double gap_to = -1,
           double burst_at = -1, size_t burst = 0) {
    Result res;
    const double dt = 1e-4;
    double game_next = 0, dev_next = 0, acc = 0, phase = 0, last = 0, sum2 = 0;
    size_t n2 = 0;
    bool first = true, burst_done = false;
    std::vector<float> in, out(size_t(rate / 100) * 2);
    for (double t = 0; t < seconds; t += dt) {
        if (t >= game_next) {   // one game frame
            game_next += 1.0 / kGameHz;
            acc += AudioStream::kInRate / kGameHz;
            size_t n = size_t(acc);
            acc -= double(n);
            if (!burst_done && burst_at >= 0 && t >= burst_at) {
                n += burst;
                burst_done = true;
            }
            in.resize(n * AudioStream::kInChannels);
            for (size_t i = 0; i < n; i++) {
                float v = float(kAmp * std::sin(phase));
                phase += 2 * kPi * kTone / AudioStream::kInRate;
                // the same tone on front left and rear left: the stereo mix is the tone itself
                in[i * 4] = v, in[i * 4 + 1] = 0, in[i * 4 + 2] = v, in[i * 4 + 3] = 0;
            }
            if (!(t >= gap_from && t < gap_to)) s.push(in.data(), n);
        }
        if (t >= dev_next) {   // one 10 ms device period
            dev_next += 0.01 / (1 + drift);
            size_t fill = s.queued();
            s.pull(out.data(), out.size() / 2);
            if (t > 1.0) {
                res.min_fill = std::min(res.min_fill, fill);
                res.max_fill = std::max(res.max_fill, fill);
            }
            for (size_t i = 0; i < out.size() / 2; i++) {
                double v = out[i * 2];
                if (!first) res.max_step = std::max(res.max_step, std::fabs(v - last));
                first = false;
                last = v;
                if (t > 1.0 && !(t >= gap_from - 0.05 && t < gap_to + 0.3) && !(burst_at >= 0 && t > burst_at - 0.05 && t < burst_at + 0.3)) {
                    sum2 += v * v;
                    n2++;
                }
            }
        }
    }
    res.rms = n2 ? std::sqrt(sum2 / double(n2)) : 0;
    return res;
}
}  // namespace

int main() {
    const double ideal_rms = kAmp / std::sqrt(2.0);
    for (int rate : {48000, 44100, 96000}) {
        // Steepest step of the tone itself at this rate, plus some room
        const double max_step = 2 * kPi * kTone / rate * kAmp * 1.15;
        for (double drift : {-0.003, 0.0, 0.003}) {
            AudioStream s;
            s.set_output_rate(rate);
            Result r = run(s, rate, drift, 20.0);
            printf("%d Hz, drift %+.1f %%: max step %.4f (limit %.4f), rms %.4f, queue %zu..%zu, adjust %+.4f %%\n", rate,
                   drift * 100, r.max_step, max_step, r.rms, r.min_fill, r.max_fill, s.rate_adjust() * 100);
            CHECK(r.max_step < max_step);
            CHECK(std::fabs(r.rms - ideal_rms) < ideal_rms * 0.01);
            CHECK(s.underruns() == 0 && s.skips() == 0);
            CHECK(r.min_fill > 0 && r.max_fill < AudioStream::kMaxFill);
        }
    }
    {   // a pause of the game (e.g. its menu) and a burst after it: fades, no clicks
        AudioStream s;
        s.set_output_rate(48000);
        Result r = run(s, 48000, 0.001, 10.0, 3.0, 3.5, 6.0, 20000);
        printf("pause and burst: max step %.4f, rms %.4f, underruns %u, skips %u\n", r.max_step, r.rms, s.underruns(),
               s.skips());
        CHECK(r.max_step < 2 * kPi * kTone / 48000 * kAmp * 1.15);
        CHECK(std::fabs(r.rms - ideal_rms) < ideal_rms * 0.01);
        CHECK(s.underruns() == 1 && s.skips() == 1);
    }
    {   // speaker layouts: a tone on the front left only
        using S = AudioStream::Speakers;
        for (S mode : {S::Stereo, S::Surround, S::Headphones}) {
            AudioStream s;
            s.set_output_rate(48000);
            std::vector<float> in(AudioStream::kTarget * 4 * 2, 0.0f);
            for (size_t i = 0; i < in.size() / 4; i++) in[i * 4] = float(kAmp * std::sin(2 * kPi * kTone * double(i) / 44100));
            s.push(in.data(), in.size() / 4);
            int ch = AudioStream::channels(mode);
            std::vector<float> out(2000 * size_t(ch));
            s.pull(out.data(), 2000, mode);
            double e[4] = {};
            for (size_t i = 1000; i < 2000; i++)
                for (int c = 0; c < ch; c++) e[c] += double(out[i * ch + c]) * out[i * ch + c];
            for (double& v : e) v = std::sqrt(v / 1000);
            printf("speakers %d: rms per channel %.4f %.4f %.4f %.4f\n", int(mode), e[0], e[1], e[2], e[3]);
            if (mode == S::Stereo) CHECK(std::fabs(e[0] - ideal_rms / 2) < 0.01 && e[1] < 1e-4);
            if (mode == S::Surround) CHECK(std::fabs(e[0] - ideal_rms * 0.7071) < 0.01 && e[1] < 1e-4 && e[2] < 1e-4 && e[3] < 1e-4);
            if (mode == S::Headphones) CHECK(e[0] > ideal_rms * 0.35 && e[0] < ideal_rms * 0.6 && e[1] > 0.01 && e[1] < e[0] * 0.5);
        }
    }
    {   // limiter: a tone far over full scale comes out whole (no clipping) and below the knee
        AudioStream s;
        s.set_output_rate(48000);
        std::vector<float> in(AudioStream::kTarget * 4 * 4, 0.0f);
        for (size_t i = 0; i < in.size() / 4; i++)
            in[i * 4] = in[i * 4 + 2] = float(2.0 * std::sin(2 * kPi * 200.0 * double(i) / 44100));
        s.push(in.data(), in.size() / 4);
        std::vector<float> out(7000 * 2);
        s.pull(out.data(), 7000);
        double peak = 0, step = 0;
        for (size_t i = 2000; i < 7000; i++) {
            peak = std::max(peak, std::fabs(double(out[i * 2])));
            step = std::max(step, std::fabs(double(out[i * 2]) - out[i * 2 - 2]));
        }
        printf("limiter: peak %.4f, max step %.4f\n", peak, step);
        CHECK(peak <= 0.751 && peak > 0.7);
        CHECK(step < 2 * kPi * 200.0 / 48000 * 0.75 * 1.2);   // a clean sine of that level, not a clipped one
    }
    CHECK(std::fabs(soft_limit(0.5f) - 0.5f) < 1e-6f && soft_limit(4.0f) <= 1.0f && soft_limit(-4.0f) >= -1.0f && soft_limit(0.9f) < 0.9f);
    if (g_failed) printf("%d checks failed\n", g_failed);
    else printf("audio_stream: all correct\n");
    return g_failed ? 1 : 0;
}

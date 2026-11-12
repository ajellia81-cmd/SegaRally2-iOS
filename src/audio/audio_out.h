// Audio output through WASAPI (shared mode). The emulation pushes 44.1 kHz
// frames into an AudioStream; a WASAPI thread resamples them to the device's
// own rate. The thread reopens the device if it goes away or the Windows
// default output changes (headphones plugged in, etc.).
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

#include "audio_stream.h"

class AudioOut {
public:
    static constexpr int kRate = AudioStream::kInRate;
    static constexpr int kChannels = AudioStream::kInChannels;

    bool start();
    void stop();
    // Called by the emulation thread: frames of kChannels floats (full scale = 1)
    void push(const float* frames, size_t count) { stream_.push(frames, count); }
    // Frames currently queued
    size_t queued() const { return stream_.queued(); }
    // Any thread; the device is reopened if the channel count changes
    void set_speakers(AudioStream::Speakers s) { speakers_ = int(s); }

private:
    void run();
    bool play_default_device();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> speakers_{int(AudioStream::Speakers::Stereo)};
    AudioStream stream_;
};

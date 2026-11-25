// iOS port: no splash video (it needs Windows' media engine); the menu shows
// the splash picture instead, as it does when the video cannot be played
#include "splash_video.h"

struct SplashVideo::Impl {};

bool SplashVideo::open(const std::wstring&) { return false; }
void SplashVideo::close() {
    delete impl_;
    impl_ = nullptr;
}
void SplashVideo::play() {}
void SplashVideo::set_volume(float) {}
bool SplashVideo::failed() const { return true; }
bool SplashVideo::ended() const { return true; }
double SplashVideo::position() const { return 0; }
bool SplashVideo::size(int&, int&) const { return false; }
bool SplashVideo::frame(int, int, std::vector<uint32_t>&, bool) { return false; }

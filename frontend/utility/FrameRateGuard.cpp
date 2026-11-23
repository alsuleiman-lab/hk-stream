#include "FrameRateGuard.hpp"

#include <widgets/OBSBasic.hpp>

#include <obs.h>

#include <cmath>

#include <QDateTime>

namespace {
constexpr int kPollMs = 3000;
constexpr size_t kWindowFrames = 900; /* ~30 s */
constexpr int kStreakNeeded = 2;
constexpr qint64 kCooldownMs = 120000;

} // namespace

FrameRateGuard::FrameRateGuard(OBSBasic *main) : QObject(main), main_(main), detector_(kWindowFrames)
{
	timer_.setInterval(kPollMs);
	connect(&timer_, &QTimer::timeout, this, &FrameRateGuard::poll);
	connect(main_, &OBSBasic::RecordingStarted, this, &FrameRateGuard::poll);
	connect(main_, &OBSBasic::StreamingStarted, this, &FrameRateGuard::poll);
	connect(main_, &OBSBasic::RecordingStopped, this, &FrameRateGuard::poll);
	connect(main_, &OBSBasic::StreamingStopped, this, &FrameRateGuard::poll);

	timer_.start();
}

FrameRateGuard::~FrameRateGuard()
{
	timer_.stop();
	detach();
}

void FrameRateGuard::RawVideoCallback(void *param, struct video_data *frame)
{
	auto *self = static_cast<FrameRateGuard *>(param);
	if (!frame || !frame->data[0] || !self->attached_.load(std::memory_order_relaxed))
		return;
	const uint64_t h = HashFrameSamples(frame->data[0], frame->linesize[0], self->width_, self->height_);
	{
		std::lock_guard<std::mutex> lock(self->mutex_);
		self->detector_.Push(h);
	}
	self->pushed_.fetch_add(1, std::memory_order_relaxed);
}

void FrameRateGuard::attach()
{
	if (attached_.load())
		return;
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return;
	width_ = ovi.output_width;
	height_ = ovi.output_height;
	fpsNum_ = ovi.fps_num;
	fpsDen_ = ovi.fps_den;
	streak_ = 0;
	lastSuggested_ = 0.0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		detector_.Reset();
	}
	attached_.store(true);
	obs_add_raw_video_callback(nullptr, &FrameRateGuard::RawVideoCallback, this);
}

void FrameRateGuard::detach()
{
	if (!attached_.exchange(false))
		return;
	obs_remove_raw_video_callback(&FrameRateGuard::RawVideoCallback, this);
}

void FrameRateGuard::poll()
{
	if (!main_->RecordingActive() && !main_->StreamingActive()) {
		detach();
		return;
	}
	if (!attached_.load()) {
		attach();
		return;
	}
	/* a video reset (workspace switch, fps change) drops the callback silently: no new frames for a whole poll => attach again */
	const uint64_t pushed = pushed_.load();
	if (pushed == lastPushed_) {
		detach();
		attach();
		return;
	}
	lastPushed_ = pushed;

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || !ovi.fps_den)
		return;
	const double canvasFps = (double)ovi.fps_num / (double)ovi.fps_den;
	if (ovi.output_width != width_ || ovi.output_height != height_ ||
	    ovi.fps_num != fpsNum_ || ovi.fps_den != fpsDen_) {
		detach();
		attach();
		return;
	}

	FrameCadenceResult r;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		r = detector_.Evaluate(canvasFps);
	}

	if (!(r.conclusive && r.mismatch)) {
		streak_ = 0;
		lastSuggested_ = 0.0;
		return;
	}
	streak_ = (std::fabs(r.suggestedFps - lastSuggested_) < 0.01) ? streak_ + 1 : 1;
	lastSuggested_ = r.suggestedFps;
	if (streak_ < kStreakNeeded || QDateTime::currentMSecsSinceEpoch() < cooldownUntilMs_)
		return;
	streak_ = 0;
	cooldownUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kCooldownMs;
	blog(LOG_WARNING,
	     "[FrameRateGuard] canvas image changes estimated at %.2f fps, canvas %.2f fps (%.1f%% identical consecutive samples). "
	     "This is not a camera input-rate measurement; preserving the selected workspace frame rate.",
	     r.uniqueFps, canvasFps, r.dupRatio * 100.0);
}

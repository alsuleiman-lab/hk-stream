#pragma once

#include <QObject>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "FrameCadence.hpp"

class OBSBasic;
struct video_data;

/*
 * HK Stream - diagnostic monitor for repeated canvas frames.
 *
 * Picture similarity cannot establish the camera's capture rate: a still scene,
 * driver repeats, and rendering stalls can all produce identical output frames.
 * Keep the user's workspace/recording rate, including a 60 fps camera recorded
 * at 30 fps. Never change it or queue a later change based on this estimate.
 * Findings are written to the log without attributing them to a specific source.
 * Subscribe only while recording/streaming, so this monitor cannot keep the
 * video output active and prevent a workspace video reset during preview.
 */
class FrameRateGuard : public QObject {
	Q_OBJECT

public:
	explicit FrameRateGuard(OBSBasic *main);
	~FrameRateGuard() override;

private slots:
	void poll();

private:
	static void RawVideoCallback(void *param, struct video_data *frame);
	void attach();
	void detach();
	OBSBasic *main_ = nullptr;
	QTimer timer_;
	std::mutex mutex_;
	FrameCadenceDetector detector_;
	std::atomic<bool> attached_{false};
	std::atomic<uint64_t> pushed_{0};
	uint64_t lastPushed_ = 0;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t fpsNum_ = 0;
	uint32_t fpsDen_ = 0;
	double lastSuggested_ = 0.0;
	int streak_ = 0;
	qint64 cooldownUntilMs_ = 0;
};

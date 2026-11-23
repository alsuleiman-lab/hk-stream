#pragma once

/*
 * HK Stream - frame cadence detector.
 *
 * Estimates repeated pictures in the composed output. Image similarity alone
 * cannot distinguish a lower capture rate from still content or delayed frames,
 * and must never be used to change the user's recording rate automatically.
 *
 * Pure C++17 on purpose (no Qt, no libobs): it is unit-tested on real recordings (QA/frame_cadence_test.cpp).
 * Feed it one hash per canvas frame (HashFrameSamples) and ask Evaluate().
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

struct FrameCadenceResult {
	bool conclusive = false;  /* enough frames and not a frozen / empty picture */
	bool mismatch = false;    /* repeated-image pattern; not proof of a lower source rate */
	double dupRatio = 0.0;    /* share of frames identical to the previous one */
	double uniqueFps = 0.0;   /* estimated rate of different sampled pictures */
	double suggestedFps = 0.0;/* nearest standard rate (0 = none matches) */
	double regularity = 0.0;  /* share of repeat runs that follow the expected cadence (1 = perfectly regular) */
};

/* Hash of a sparse sample of the first (luma) plane: every 8th row and 8th column. Identical frames give identical hashes. */
inline uint64_t HashFrameSamples(const uint8_t *plane, uint32_t linesize, uint32_t width, uint32_t height)
{
	uint64_t h = 1469598103934665603ull;
	for (uint32_t y = 0; y < height; y += 8) {
		const uint8_t *row = plane + (size_t)y * linesize;
		for (uint32_t x = 0; x < width; x += 8) {
			h ^= row[x];
			h *= 1099511628211ull;
		}
	}
	return h;
}

class FrameCadenceDetector {
public:
	static constexpr size_t kMinFrames = 300; /* ~10 s at 30 fps */

	explicit FrameCadenceDetector(size_t window = 36000) : window_(window) {}

	void Reset() { hashes_.clear(); }
	/* keeps the last `window` frames (a sliding window) */
	void Push(uint64_t hash)
	{
		if (hashes_.size() >= window_)
			hashes_.pop_front();
		hashes_.push_back(hash);
	}
	size_t Count() const { return hashes_.size(); }

	FrameCadenceResult Evaluate(double canvasFps) const
	{
		FrameCadenceResult r;
		const size_t n = hashes_.size();
		if (n < kMinFrames || canvasFps <= 0.0)
			return r;

		/* length of every run of identical frames (1 = a normal frame, 2 = one repeat, ...) */
		std::vector<int> runs;
		int run = 1, dups = 0;
		for (size_t i = 1; i < n; i++) {
			if (hashes_[i] == hashes_[i - 1]) {
				run++;
				dups++;
			} else {
				runs.push_back(run);
				run = 1;
			}
		}
		runs.push_back(run);

		r.dupRatio = (double)dups / (double)(n - 1);
		if (r.dupRatio > 0.6)
			return r; /* frozen picture / no signal / still image: nothing to judge */
		r.conclusive = true;
		if (r.dupRatio < 0.04)
			return r; /* clean: no repeats worth reporting */

		const double meanRun = (double)n / (double)runs.size(); /* canvas frames per distinct picture */
		r.uniqueFps = canvasFps / meanRun;

		/* a real lower frame rate repeats on a steady cadence: every run is floor(meanRun) or ceil(meanRun) frames long */
		const int lo = (int)std::floor(meanRun), hi = (int)std::ceil(meanRun);
		size_t regular = 0;
		for (int v : runs)
			if (v == lo || v == hi)
				regular++;
		r.regularity = (double)regular / (double)runs.size();

		static const double kStandard[] = {23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0};
		double closestError = 0.045;
		for (double s : kStandard) {
			const double error = std::fabs(r.uniqueFps - s) / s;
			if (error <= closestError) {
				closestError = error;
				r.suggestedFps = s;
			}
		}
		r.mismatch = r.regularity >= 0.85 && r.suggestedFps > 0.0 && (canvasFps - r.suggestedFps) / canvasFps >= 0.08;
		return r;
	}

private:
	std::deque<uint64_t> hashes_;
	size_t window_;
};

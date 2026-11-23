#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <atomic>
#include <obs.h>
#include <obs.hpp>
#include "workspaces/WorkspaceModel.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

class SmartDirector : public QObject {
	Q_OBJECT

public:
	static SmartDirector &Instance();

	void SetWorkspaceConfig(const std::string &workspaceId,
				const std::string &baseScene,
				const std::map<std::string, OBSInteractiveSceneConfig> &interactiveScenes);

	void SetEnabled(bool enabled);
	bool IsEnabled() const { return enabled_; }

	void SetCooldownMs(int ms) { cooldownMs_ = ms; }
	int GetCooldownMs() const { return cooldownMs_; }

	void ResetState();
	void SelectScene(const std::string &logicalScene);

signals:
	void StatusChanged(bool active, const QString &currentScene);
	void SceneSwitched(const QString &fromScene, const QString &toScene, const QString &reason);

private slots:
	void OnTimerTick();

private:
	SmartDirector();
	~SmartDirector();

	bool enabled_ = true;
	int cooldownMs_ = 500; // Return to the fixed fallback after half a second of inactivity.
	std::string currentWorkspaceId_;
	std::string baseScene_;
	std::map<std::string, OBSInteractiveSceneConfig> interactiveScenes_; // scene -> trigger + fixed fallback

	// Audio monitoring per source
	struct SourceAudioMonitor {
		obs_volmeter_t *volmeter = nullptr;
		std::atomic<int64_t> lastActiveMs{0};
	};
	std::map<std::string, SourceAudioMonitor> audioMonitors_;
	uint32_t monitoredWidth_ = 0;
	uint32_t monitoredHeight_ = 0;

	// Activity and window tracking per source
	struct SourceTrackingInfo {
		std::chrono::steady_clock::time_point lastMouseActiveTime;
		HWND trackedHwnd = nullptr;
		DWORD trackedPid = 0;
		std::string lastWindowSetting;
		std::chrono::steady_clock::time_point lastAudioPoll;
		std::chrono::steady_clock::time_point lastWindowAudioTime;
		bool windowAudioActive = false;
		bool windowWarningLogged = false;
	};
	std::map<std::string, SourceTrackingInfo> sourceTracking_;

	// State
	bool inInteractiveMode_ = false;
	std::string activeInteractiveScene_;
	std::string activeInteractiveFallback_;
	std::string expectedSceneSwitch_;
	std::string lastObservedScene_;
	QTimer *timer_ = nullptr;

	void UpdateAudioMonitors();
	void ClearAudioMonitors();
	static void AudioCallback(void *param,
				  const float magnitude[MAX_AUDIO_CHANNELS],
				  const float peak[MAX_AUDIO_CHANNELS],
				  const float input_peak[MAX_AUDIO_CHANNELS]);

	bool CheckMouseOverSource(const std::string &sceneName, const std::string &sourceName);
	bool CheckWindowAudio(const std::string &sourceName);
	obs_source_t *ResolveTriggerSource(const std::string &sceneName, const std::string &sourceName) const;
};

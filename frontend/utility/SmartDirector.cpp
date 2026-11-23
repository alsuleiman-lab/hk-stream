#include "SmartDirector.hpp"
#include "workspaces/WorkspaceModel.hpp"
#include "dialogs/OBSBasicInteraction.hpp"

#include <obs-frontend-api.h>
#include <util/base.h>
#include <util/platform.h>
#include <util/windows/window-helpers.h>
#include <util/windows/ComPtr.hpp>
#include <windows.h>
#include <tlhelp32.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <psapi.h>
#include <QFileInfo>
#include <QDebug>
#include <QApplication>
#include <algorithm>
#include <limits>
#include <set>

#include "moc_SmartDirector.cpp"

namespace {
bool WindowProcessHasAudio(DWORD processId)
{
	if (!processId) return false;
	// Browsers and players can emit audio from a child process.
	std::set<DWORD> processes{processId};
	std::map<DWORD, DWORD> parents;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
		if (Process32FirstW(snapshot, &entry)) {
			do { parents[entry.th32ProcessID] = entry.th32ParentProcessID; }
			while (Process32NextW(snapshot, &entry));
		}
		CloseHandle(snapshot);
		bool changed;
		do {
			changed = false;
			for (const auto &entry : parents)
				if (processes.count(entry.second) && processes.insert(entry.first).second) changed = true;
		} while (changed);
	}
	const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
	struct ComScope {
		bool owns;
		~ComScope() { if (owns) CoUninitialize(); }
	} scope{SUCCEEDED(initialized)};
	ComPtr<IMMDeviceEnumerator> enumerator;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
		__uuidof(IMMDeviceEnumerator), (void **)enumerator.Assign()))) return false;
	ComPtr<IMMDeviceCollection> devices;
	if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.Assign()))) return false;
	UINT deviceCount = 0;
	if (FAILED(devices->GetCount(&deviceCount))) return false;
	for (UINT d = 0; d < deviceCount; ++d) {
		ComPtr<IMMDevice> device;
		ComPtr<IAudioSessionManager2> manager;
		ComPtr<IAudioSessionEnumerator> sessions;
		if (FAILED(devices->Item(d, device.Assign())) ||
		    FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr,
			(void **)manager.Assign())) || FAILED(manager->GetSessionEnumerator(sessions.Assign()))) continue;
		int count = 0;
		if (FAILED(sessions->GetCount(&count))) continue;
		for (int i = 0; i < count; ++i) {
			ComPtr<IAudioSessionControl> session;
			ComPtr<IAudioSessionControl2> control;
			ComPtr<IAudioMeterInformation> meter;
			if (FAILED(sessions->GetSession(i, session.Assign())) ||
			    FAILED(session->QueryInterface(__uuidof(IAudioSessionControl2), (void **)control.Assign()))) continue;
			DWORD pid = 0;
			if (FAILED(control->GetProcessId(&pid)) || !processes.count(pid)) continue;
			if (FAILED(session->QueryInterface(__uuidof(IAudioMeterInformation), (void **)meter.Assign()))) continue;
			float peak = 0;
			// Windows session meters use linear amplitude (about -36 dB).
			if (SUCCEEDED(meter->GetPeakValue(&peak)) && peak > 0.015f) return true;
		}
	}
	return false;
}

struct TriggerItemSearch {
	std::string name;
	std::string logicalName;
	obs_source_t *match = nullptr;
};

bool FindVisibleTriggerItem(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	auto &search = *static_cast<TriggerItemSearch *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (obs_sceneitem_visible(item) && name &&
	    (search.name == name || OBSWorkspaceManager::GetLogicalSourceName(name) == search.logicalName)) {
		search.match = obs_source_get_ref(source);
		return false;
	}
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, FindVisibleTriggerItem, param);
	return search.match == nullptr;
}
}

SmartDirector &SmartDirector::Instance()
{
	static SmartDirector s_instance;
	return s_instance;
}

SmartDirector::SmartDirector()
{
	timer_ = new QTimer(this);
	timer_->setInterval(100); // Check every 100ms
	connect(timer_, &QTimer::timeout, this, &SmartDirector::OnTimerTick);
	timer_->start();
}

SmartDirector::~SmartDirector()
{
	ClearAudioMonitors();
}

void SmartDirector::SetEnabled(bool enabled)
{
	if (enabled_ != enabled) {
		enabled_ = enabled;
		if (!enabled_) {
			ResetState();
			ClearAudioMonitors();
		}
	}
}

void SmartDirector::SetWorkspaceConfig(const std::string &workspaceId,
				      const std::string &baseScene,
				      const std::map<std::string, OBSInteractiveSceneConfig> &interactiveScenes)
{
	const bool sameConfig = currentWorkspaceId_ == workspaceId && baseScene_ == baseScene &&
		interactiveScenes_.size() == interactiveScenes.size() &&
		std::equal(interactiveScenes_.begin(), interactiveScenes_.end(), interactiveScenes.begin(),
			[](const auto &a, const auto &b) {
				return a.first == b.first && a.second.source_name == b.second.source_name &&
				       a.second.fallback_scene == b.second.fallback_scene &&
				       a.second.enabled == b.second.enabled;
			});
	if (sameConfig) return;
	const std::string armedScene = activeInteractiveScene_;
	const bool keepArmed = enabled_ && currentWorkspaceId_ == workspaceId && !armedScene.empty() && interactiveScenes.count(armedScene) &&
		interactiveScenes.at(armedScene).enabled;
	currentWorkspaceId_ = workspaceId;
	baseScene_ = baseScene;
	interactiveScenes_ = interactiveScenes;

	ClearAudioMonitors();
	sourceTracking_.clear();
	if (keepArmed) {
		activeInteractiveFallback_ = interactiveScenes_.at(armedScene).fallback_scene.empty()
			? baseScene_ : interactiveScenes_.at(armedScene).fallback_scene;
		inInteractiveMode_ = false;
		expectedSceneSwitch_.clear();
		lastObservedScene_ = armedScene;
		UpdateAudioMonitors();
	} else {
		ResetState();
		if (enabled_)
			UpdateAudioMonitors();
	}
}

void SmartDirector::ResetState()
{
	inInteractiveMode_ = false;
	activeInteractiveScene_.clear();
	activeInteractiveFallback_.clear();
	expectedSceneSwitch_.clear();
	lastObservedScene_.clear();
	auto now = std::chrono::steady_clock::now();
	for (auto &pair : sourceTracking_) {
		pair.second.lastMouseActiveTime = now - std::chrono::seconds(10);
		pair.second.lastWindowAudioTime = now - std::chrono::seconds(10);
		pair.second.lastAudioPoll = {};
		pair.second.windowAudioActive = false;
	}
	for (auto &pair : audioMonitors_) {
		pair.second.lastActiveMs.store(0);
	}
}

void SmartDirector::SelectScene(const std::string &logicalScene)
{
	ResetState();
	ClearAudioMonitors();
	lastObservedScene_ = logicalScene;
	if (!enabled_)
		return;
	const auto selected = interactiveScenes_.find(logicalScene);
	if (selected != interactiveScenes_.end() && selected->second.enabled) {
		activeInteractiveScene_ = selected->first;
		activeInteractiveFallback_ = selected->second.fallback_scene.empty()
			? baseScene_ : selected->second.fallback_scene;
		blog(LOG_INFO, "[SmartDirector] Armed scene '%s', trigger '%s', fallback '%s'", logicalScene.c_str(),
			selected->second.source_name.c_str(), activeInteractiveFallback_.c_str());
	}
	UpdateAudioMonitors();
}

obs_source_t *SmartDirector::ResolveTriggerSource(const std::string &sceneName, const std::string &sourceName) const
{
	// Resolve the trigger inside this scene's current size, including isolated copies.
	obs_video_info video{};
	if (obs_get_video_info(&video)) {
		const std::string aspect = OBSWorkspaceManager::GetAspectRatioKey(video.base_width, video.base_height);
		const std::string backing = OBSWorkspaceManager::GetBackingSceneName(
			sceneName, aspect, currentWorkspaceId_, video.base_width, video.base_height);
		OBSSourceAutoRelease sceneSource = obs_get_source_by_name(backing.c_str());
		obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
		if (scene) {
			TriggerItemSearch search{sourceName, OBSWorkspaceManager::GetLogicalSourceName(sourceName)};
			obs_scene_enum_items(scene, FindVisibleTriggerItem, &search);
			// Do not resolve a missing or hidden scene item through a global source
			// lookup, which could select another scene's isolated copy.
			return search.match;
		}
	}
	// Workspace triggers must resolve only within their exact backing scene.
	// A global name fallback can silently bind the landscape copy while the
	// portrait scene is active (or cross into another workspace).
	return nullptr;
}

void SmartDirector::ClearAudioMonitors()
{
	for (auto &pair : audioMonitors_) {
		if (pair.second.volmeter) {
			obs_volmeter_remove_callback(pair.second.volmeter, &SmartDirector::AudioCallback, &pair.second);
			obs_volmeter_detach_source(pair.second.volmeter);
			obs_volmeter_destroy(pair.second.volmeter);
		}
	}
	audioMonitors_.clear();
}

void SmartDirector::UpdateAudioMonitors()
{
	ClearAudioMonitors();
	obs_video_info video{};
	if (obs_get_video_info(&video)) {
		monitoredWidth_ = video.base_width;
		monitoredHeight_ = video.base_height;
	}

	for (const auto &pair : interactiveScenes_) {
		if (!pair.second.enabled) continue;
		const std::string &srcName = pair.second.source_name;
		if (srcName.empty() || audioMonitors_.find(srcName) != audioMonitors_.end())
			continue;

		obs_source_t *src = ResolveTriggerSource(pair.first, srcName);
		if (!src) continue;
		if (strcmp(obs_source_get_id(src), "window_capture") == 0) {
			obs_data_t *settings = obs_source_get_settings(src);
			if (!obs_data_get_bool(settings, "match_app_identity")) {
				// A browser's title changes with its tab. The interactive capture
				// follows its application and window class instead of page text.
				obs_data_set_bool(settings, "match_app_identity", true);
				obs_source_update(src, settings);
			}
			obs_data_release(settings);
		}

		uint32_t flags = obs_source_get_output_flags(src);
		blog(LOG_INFO, "[SmartDirector] Resolved trigger '%s' to '%s' (audio: %s)", srcName.c_str(),
			obs_source_get_name(src), (flags & OBS_SOURCE_AUDIO) ? "yes" : "no");
		if (flags & OBS_SOURCE_AUDIO) {
			obs_volmeter_t *vm = obs_volmeter_create(OBS_FADER_LOG);
			if (vm) {
				SourceAudioMonitor &mon = audioMonitors_[srcName];
				mon.volmeter = vm;
				mon.lastActiveMs.store(0);
				obs_volmeter_attach_source(vm, src);
				obs_volmeter_add_callback(vm, &SmartDirector::AudioCallback, &mon);
			}
		}
		obs_source_release(src);
	}
}

void SmartDirector::AudioCallback(void *param,
				  const float /*magnitude*/[MAX_AUDIO_CHANNELS],
				  const float peak[MAX_AUDIO_CHANNELS],
				  const float /*input_peak*/[MAX_AUDIO_CHANNELS])
{
	auto *mon = static_cast<SourceAudioMonitor *>(param);
	if (!mon) return;

	float maxPeak = -std::numeric_limits<float>::infinity();
	for (int i = 0; i < MAX_AUDIO_CHANNELS; ++i) {
		if (peak[i] > maxPeak) maxPeak = peak[i];
	}
	// The meter callback supplies dB, not linear amplitudes.
	if (maxPeak > -36.0f) {
		mon->lastActiveMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}
}

bool SmartDirector::CheckMouseOverSource(const std::string &sceneName, const std::string &sourceName)
{
	if (sourceName.empty()) return false;

	obs_source_t *src = ResolveTriggerSource(sceneName, sourceName);
	if (!src) return false;

	POINT pt = {};
	if (!GetCursorPos(&pt)) {
		obs_source_release(src);
		return false;
	}
	const HWND hwndUnder = WindowFromPoint(pt);
	// HK's browser interaction window has no capture-window setting.
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		auto *interaction = qobject_cast<OBSBasicInteraction *>(widget);
		if (!interaction || !interaction->isVisible() || interaction->isMinimized() ||
		    interaction->GetSource() != src) continue;
		const HWND window = reinterpret_cast<HWND>(interaction->winId());
		if (hwndUnder == window || GetAncestor(hwndUnder, GA_ROOT) == window || IsChild(window, hwndUnder)) {
			obs_source_release(src);
			return true;
		}
	}

	obs_data_t *settings = obs_source_get_settings(src);
	const char *win_setting = obs_data_get_string(settings, "window");
	// Prefer the window the capture has actually acquired: browser titles change
	// while its HWND remains the same, and saved titles can be stale.
	calldata_t hooked = {};
	const bool hasHookInfo = strcmp(obs_source_get_id(src), "window_capture") == 0 &&
		proc_handler_call(obs_source_get_proc_handler(src), "get_hooked", &hooked) &&
		calldata_bool(&hooked, "hooked");

	bool isOver = false;

	if (win_setting && *win_setting) {
		auto &tracking = sourceTracking_[sourceName];
		std::string curSettingStr(win_setting);
		if (tracking.lastWindowSetting != curSettingStr) {
			tracking.lastWindowSetting = curSettingStr;
			tracking.trackedHwnd = nullptr;
			tracking.trackedPid = 0;
			tracking.windowWarningLogged = false;
		}

		// Decode window settings (title, class, executable)
		char *win_class = nullptr;
		char *win_title = nullptr;
		char *win_exe = nullptr;
		ms_build_window_strings(win_setting, &win_class, &win_title, &win_exe);

		QString targetTitle = win_title ? QString::fromUtf8(win_title) : "";
		QString targetClass = win_class ? QString::fromUtf8(win_class) : "";
		QString targetExe   = win_exe   ? QString::fromUtf8(win_exe)   : "";
		if (hasHookInfo) {
			targetTitle = QString::fromUtf8(calldata_string(&hooked, "title"));
			targetClass = QString::fromUtf8(calldata_string(&hooked, "class"));
			targetExe = QString::fromUtf8(calldata_string(&hooked, "executable"));
		}

		// Fallback manual parse if ms_build_window_strings didn't find parts
		if (targetExe.isEmpty() || targetTitle.isEmpty()) {
			QString winStr = QString::fromUtf8(win_setting);
			QStringList parts = winStr.split(':');
			if (parts.size() >= 3) {
				if (targetTitle.isEmpty()) targetTitle = parts[0];
				if (targetClass.isEmpty()) targetClass = parts[1];
				if (targetExe.isEmpty()) targetExe = parts.last();
			} else if (!parts.isEmpty() && targetExe.isEmpty()) {
				targetExe = parts.last();
			}
		}

		// Validate previously tracked HWND
		if (tracking.trackedHwnd) {
			if (!IsWindow(tracking.trackedHwnd)) {
				tracking.trackedHwnd = nullptr;
				tracking.trackedPid = 0;
			} else {
				DWORD pid = 0;
				GetWindowThreadProcessId(tracking.trackedHwnd, &pid);
				if (pid != tracking.trackedPid || pid == 0) {
					tracking.trackedHwnd = nullptr;
					tracking.trackedPid = 0;
				}
			}
		}

		// If no valid HWND cached, search using OBS window helper
		if (!tracking.trackedHwnd) {
			int priority = (int)obs_data_get_int(settings, "priority");

			const QByteArray classBytes = targetClass.toUtf8();
			const QByteArray titleBytes = targetTitle.toUtf8();
			const QByteArray exeBytes = targetExe.toUtf8();
			const char *c_class = targetClass.isEmpty() ? nullptr : classBytes.constData();
			const char *c_title = targetTitle.isEmpty() ? nullptr : titleBytes.constData();
			const char *c_exe = targetExe.isEmpty() ? nullptr : exeBytes.constData();

			HWND found = ms_find_window_top_level(INCLUDE_MINIMIZED, (enum window_priority)priority,
			                                      c_class, c_title, c_exe);
			if (!found) {
				found = ms_find_window(INCLUDE_MINIMIZED, (enum window_priority)priority,
				                       c_class, c_title, c_exe);
			}

			// If still not found, do a precise title/exe scan among top-level windows
			if (!found && !targetExe.isEmpty()) {
				struct EnumContext {
					QString exe;
					QString title;
					QString className;
					HWND bestHwnd = nullptr;
					int bestScore = -1;
				} ctx;
				ctx.exe = targetExe;
				ctx.title = targetTitle;
				ctx.className = targetClass;

				EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
					if (!IsWindowVisible(hwnd) && !IsIconic(hwnd))
						return TRUE;

					EnumContext *c = reinterpret_cast<EnumContext *>(lParam);
					DWORD pid = 0;
					GetWindowThreadProcessId(hwnd, &pid);
					if (pid == 0) return TRUE;

					HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
					if (!hProc) return TRUE;

					wchar_t procPath[MAX_PATH] = {0};
					DWORD sz = MAX_PATH;
					bool matchExe = false;
					if (QueryFullProcessImageNameW(hProc, 0, procPath, &sz)) {
						QString p = QString::fromWCharArray(procPath);
						if (p.endsWith(c->exe, Qt::CaseInsensitive)) {
							matchExe = true;
						}
					}
					CloseHandle(hProc);
					if (!matchExe) return TRUE;

					if (!c->className.isEmpty()) {
						wchar_t wClass[256] = {0};
						GetClassNameW(hwnd, wClass, 256);
						QString cls = QString::fromWCharArray(wClass);
						if (cls.compare(c->className, Qt::CaseInsensitive) != 0) {
							return TRUE;
						}
					}

					wchar_t wTitle[512] = {0};
					int tLen = GetWindowTextW(hwnd, wTitle, 512);
					if (tLen <= 0) return TRUE;

					QString title = QString::fromWCharArray(wTitle);
					int score = 0;
					if (!c->title.isEmpty()) {
						if (title.compare(c->title, Qt::CaseInsensitive) == 0) {
							score = 100;
						} else if (title.contains(c->title, Qt::CaseInsensitive) ||
						           c->title.contains(title, Qt::CaseInsensitive)) {
							score = 60;
						} else {
							// The configured capture identifies the app; its title may change
							// whenever the browser navigates or switches tabs.
							score = 20;
						}
					} else {
						score = 10;
					}

					if (score > c->bestScore) {
						c->bestScore = score;
						c->bestHwnd = hwnd;
					}
					return (c->bestScore < 100);
				}, reinterpret_cast<LPARAM>(&ctx));

				found = ctx.bestHwnd;
			}

			if (found) {
				tracking.trackedHwnd = found;
				GetWindowThreadProcessId(found, &tracking.trackedPid);
				blog(LOG_INFO, "[SmartDirector] Window acquired for '%s' (PID %lu)", sourceName.c_str(),
					static_cast<unsigned long>(tracking.trackedPid));
				tracking.windowWarningLogged = false;
			} else if (!tracking.windowWarningLogged) {
				blog(LOG_WARNING, "[SmartDirector] Selected capture window is unavailable for '%s'", sourceName.c_str());
				tracking.windowWarningLogged = true;
			}
		}

		bfree(win_class);
		bfree(win_title);
		bfree(win_exe);

		if (tracking.trackedHwnd) {
			HWND targetHwnd = tracking.trackedHwnd;

			auto isTargetWindow = [&](HWND h) -> bool {
				if (!h || !targetHwnd) return false;
				if (h == targetHwnd) return true;
				HWND root = GetAncestor(h, GA_ROOT);
				if (root == targetHwnd) return true;
				HWND rootOwner = GetAncestor(h, GA_ROOTOWNER);
				if (rootOwner == targetHwnd) return true;
				if (IsChild(targetHwnd, h)) return true;
				// Match the selected application window by class and executable.
				// Browser titles identify tabs and are deliberately not part of this test.
				HWND candidate = root ? root : h;
				wchar_t className[256] = {};
				if (GetClassNameW(candidate, className, 256) <= 0 ||
				    QString::fromWCharArray(className).compare(targetClass, Qt::CaseInsensitive) != 0)
					return false;
				struct dstr executable = {};
				const bool haveExecutable = ms_get_window_exe(&executable, candidate);
				const QString actualExe = haveExecutable ? QString::fromUtf8(executable.array) : QString();
				dstr_free(&executable);
				return !targetExe.isEmpty() && actualExe.endsWith(targetExe, Qt::CaseInsensitive);
			};

			if (isTargetWindow(hwndUnder)) {
				isOver = true;
				HWND hoveredRoot = GetAncestor(hwndUnder, GA_ROOT);
				HWND audioWindow = hoveredRoot ? hoveredRoot : hwndUnder;
				GetWindowThreadProcessId(audioWindow, &tracking.trackedPid);
				tracking.trackedHwnd = audioWindow;
			}
		}
	}

	obs_data_release(settings);
	calldata_free(&hooked);
	obs_source_release(src);
	return isOver;
}

bool SmartDirector::CheckWindowAudio(const std::string &sourceName)
{
	auto found = sourceTracking_.find(sourceName);
	if (found == sourceTracking_.end()) return false;
	auto &tracking = found->second;
	if (!tracking.trackedHwnd || !IsWindow(tracking.trackedHwnd)) return false;
	const auto now = std::chrono::steady_clock::now();
	if (now - tracking.lastAudioPoll >= std::chrono::milliseconds(400)) {
		tracking.lastAudioPoll = now;
		tracking.windowAudioActive = WindowProcessHasAudio(tracking.trackedPid);
		if (tracking.windowAudioActive) tracking.lastWindowAudioTime = now;
	}
	return tracking.windowAudioActive;
}

void SmartDirector::OnTimerTick()
{
	if (!enabled_ || currentWorkspaceId_.empty() || interactiveScenes_.empty())
		return;

	const auto now = std::chrono::steady_clock::now();
	obs_source_t *curSceneSrc = obs_frontend_get_current_scene();
	const std::string currentBackingName = curSceneSrc ? obs_source_get_name(curSceneSrc) : "";
	if (curSceneSrc)
		obs_source_release(curSceneSrc);
	if (currentBackingName.empty())
		return;

	const std::string currentLogicalName = OBSWorkspaceManager::GetLogicalSceneName(currentBackingName);
	// Consume an automatic switch once, when the observed scene changes.
	// Remaining on its fallback keeps the selected interactive pair armed.
	if (lastObservedScene_ != currentLogicalName) {
		if (expectedSceneSwitch_ == currentLogicalName) {
			expectedSceneSwitch_.clear();
			lastObservedScene_ = currentLogicalName;
		} else {
			SelectScene(currentLogicalName);
		}
	}
	obs_video_info monitoredVideo{};
	if (obs_get_video_info(&monitoredVideo) &&
	    (monitoredWidth_ != monitoredVideo.base_width || monitoredHeight_ != monitoredVideo.base_height))
		UpdateAudioMonitors();

	// Discover a new pair only from a fixed scene. While an interactive scene is
	// on air, its own trigger controls the return below; another active source
	// must not steal control and make two playing sources alternate scenes.
	const auto currentPair = interactiveScenes_.find(currentLogicalName);
	const bool currentSceneIsInteractive = currentPair != interactiveScenes_.end() && currentPair->second.enabled;
	if (!currentSceneIsInteractive) {
		const int64_t scanNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
		for (const auto &candidate : interactiveScenes_) {
			if (candidate.first == currentLogicalName || !candidate.second.enabled ||
			    candidate.second.source_name.empty() || candidate.second.fallback_scene.empty() ||
			    candidate.second.fallback_scene == candidate.first)
				continue;

			const std::string &candidateSourceName = candidate.second.source_name;
			OBSSourceAutoRelease candidateSource = ResolveTriggerSource(candidate.first, candidateSourceName);
			if (!candidateSource)
				continue;
			if (CheckMouseOverSource(candidate.first, candidateSourceName))
				sourceTracking_[candidateSourceName].lastMouseActiveTime = now;

			const auto mouse = sourceTracking_.find(candidateSourceName);
			const auto audio = audioMonitors_.find(candidateSourceName);
			const int64_t lastAudio = audio != audioMonitors_.end() ? audio->second.lastActiveMs.load() : 0;
			const bool mouseActive = mouse != sourceTracking_.end() &&
				std::chrono::duration_cast<std::chrono::milliseconds>(
					now - mouse->second.lastMouseActiveTime).count() < cooldownMs_;
			const bool audioActive = (lastAudio > 0 && scanNowMs - lastAudio < cooldownMs_) ||
				CheckWindowAudio(candidateSourceName);
			const bool mediaPlaying = obs_source_active(candidateSource) &&
				obs_source_media_get_state(candidateSource) == OBS_MEDIA_STATE_PLAYING;
			if (mouseActive || audioActive || mediaPlaying) {
				activeInteractiveScene_ = candidate.first;
				activeInteractiveFallback_ = candidate.second.fallback_scene;
				inInteractiveMode_ = true;
				UpdateAudioMonitors();
				break;
			}
		}
	}
	if (activeInteractiveScene_.empty()) return;

	auto configIt = interactiveScenes_.find(activeInteractiveScene_);
	if (configIt == interactiveScenes_.end() || activeInteractiveFallback_.empty() ||
	    activeInteractiveFallback_ == activeInteractiveScene_) {
		ResetState();
		return;
	}
	const std::string &sourceName = configIt->second.source_name;
	if (sourceName.empty())
		return;
	OBSSourceAutoRelease triggerSource = ResolveTriggerSource(activeInteractiveScene_, sourceName);
	if (!triggerSource)
		inInteractiveMode_ = false;

	if (triggerSource && CheckMouseOverSource(activeInteractiveScene_, sourceName)) {
		sourceTracking_[sourceName].lastMouseActiveTime = now;
	}

	const auto elapsedMs = [now](const auto &timePoint) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(now - timePoint).count();
	};
	const auto mouseIt = sourceTracking_.find(sourceName);
	const auto audioIt = audioMonitors_.find(sourceName);
	const bool mouseActive = triggerSource && mouseIt != sourceTracking_.end() &&
		elapsedMs(mouseIt->second.lastMouseActiveTime) < cooldownMs_;
	const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
	const int64_t lastAudioMs = audioIt != audioMonitors_.end() ? audioIt->second.lastActiveMs.load() : 0;
	const bool audioActive = triggerSource &&
		((lastAudioMs > 0 && nowMs - lastAudioMs < cooldownMs_) || CheckWindowAudio(sourceName));
	const bool mediaPlaying = triggerSource && obs_source_active(triggerSource) &&
		obs_source_media_get_state(triggerSource) == OBS_MEDIA_STATE_PLAYING;
	const bool recentlyActive =
		(mouseIt != sourceTracking_.end() && elapsedMs(mouseIt->second.lastMouseActiveTime) < cooldownMs_) ||
		(mouseIt != sourceTracking_.end() && elapsedMs(mouseIt->second.lastWindowAudioTime) < cooldownMs_) ||
		(lastAudioMs > 0 && nowMs - lastAudioMs < cooldownMs_);

	if (audioActive || mouseActive || mediaPlaying)
		inInteractiveMode_ = true;
	else if (!recentlyActive)
		inInteractiveMode_ = false;

	const std::string targetLogical = inInteractiveMode_ ? activeInteractiveScene_ : activeInteractiveFallback_;
	std::string targetBacking = targetLogical;
	obs_video_info ovi{};
	if (obs_get_video_info(&ovi)) {
		const std::string aspect = OBSWorkspaceManager::GetAspectRatioKey(ovi.base_width, ovi.base_height);
		const std::string candidate = OBSWorkspaceManager::GetBackingSceneName(
			targetLogical, aspect, currentWorkspaceId_, ovi.base_width, ovi.base_height);
		obs_source_t *candidateSource = obs_get_source_by_name(candidate.c_str());
		if (candidateSource) {
			obs_source_release(candidateSource);
			targetBacking = candidate;
		}
	}

	if (currentBackingName == targetBacking || currentLogicalName == targetLogical)
		return;
	obs_source_t *target = obs_get_source_by_name(targetBacking.c_str());
	if (!target)
		return;
	expectedSceneSwitch_ = targetLogical;
	obs_frontend_set_current_scene(target);
	obs_source_release(target);
	emit SceneSwitched(QString::fromStdString(currentBackingName), QString::fromStdString(targetBacking),
			   inInteractiveMode_ ? "Interactive source active" : "Interactive source idle");
	blog(LOG_INFO, "[SmartDirector] Switched interactive pair to %s (source %s)", targetBacking.c_str(),
	     inInteractiveMode_ ? "active" : "idle");
}

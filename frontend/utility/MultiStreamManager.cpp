#include "MultiStreamManager.hpp"
#include "StreamTargetService.hpp"
#include "workspaces/WorkspaceModel.hpp"

#include <OBSApp.hpp>
#include <util/base.h>
#include <obs.h>
#include <obs-frontend-api.h>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

MultiStreamManager &MultiStreamManager::Instance()
{
	static MultiStreamManager instance;
	return instance;
}

MultiStreamManager::MultiStreamManager() : stoppedOutputsTimer_(this)
{
	stoppedOutputsTimer_.setInterval(100);
	connect(&stoppedOutputsTimer_, &QTimer::timeout, this, [this] { ReleaseStoppedOutputs(); });
	InitDefaultTargets();
	Load();
}

MultiStreamManager::~MultiStreamManager()
{
	stoppedOutputsTimer_.stop();
	StopAllOutputs(true);
}

void MultiStreamManager::InitDefaultTargets()
{
	targets_.clear();

	MultiStreamTarget yt;
	yt.id = "youtube";
	yt.name = QString::fromUtf8("يوتيوب (YouTube)");
	yt.icon = QString::fromUtf8("🔴");
	yt.defaultServer = QString::fromLatin1(HKStream::YouTubeServer);
	yt.server = yt.defaultServer;
	yt.streamKey = "";
	yt.enabled = true;
	yt.cx = 1920;
	yt.cy = 1080;
	targets_["youtube"] = yt;

	MultiStreamTarget fb;
	fb.id = "facebook";
	fb.name = QString::fromUtf8("فيسبوك (Facebook)");
	fb.icon = QString::fromUtf8("🔵");
	fb.defaultServer = "rtmps://live-api-s.facebook.com:443/rtmp/";
	fb.server = fb.defaultServer;
	fb.streamKey = "";
	fb.enabled = true;
	fb.cx = 1920;
	fb.cy = 1080;
	targets_["facebook"] = fb;

	MultiStreamTarget tt;
	tt.id = "tiktok";
	tt.name = QString::fromUtf8("تيك توك (TikTok)");
	tt.icon = QString::fromUtf8("🎵");
	tt.defaultServer = "rtmp://live-push.tiktok.com/live/";
	tt.server = tt.defaultServer;
	tt.streamKey = "";
	tt.enabled = true;
	tt.cx = 1080;
	tt.cy = 1920; // 9:16 vertical
	targets_["tiktok"] = tt;

	MultiStreamTarget ig;
	ig.id = "instagram";
	ig.name = QString::fromUtf8("إنستغرام (Instagram)");
	ig.icon = QString::fromUtf8("🟣");
	ig.defaultServer = "rtmps://live-upload.instagram.com:443/rtmp/";
	ig.server = ig.defaultServer;
	ig.streamKey = "";
	ig.enabled = false;
	ig.cx = 1080;
	ig.cy = 1920; // 9:16 vertical
	targets_["instagram"] = ig;
}

QString MultiStreamManager::GetConfigFilePath() const
{
	QString mainPath = QString::fromStdString(OBSWorkspaceManager::GetWorkspaceFilePath("stream_destinations.json"));
	if (QFile::exists(mainPath)) {
		return mainPath;
	}
	QString appDir = QCoreApplication::applicationDirPath();
	QString cand = appDir + "/../../workspaces/stream_destinations.json";
	if (QFile::exists(cand)) {
		return cand;
	}
	return mainPath;
}

void MultiStreamManager::Load()
{
	QString filePath = GetConfigFilePath();
	QFile file(filePath);
	if (file.open(QIODevice::ReadOnly)) {
		QByteArray data = file.readAll();
		file.close();

		QJsonDocument doc = QJsonDocument::fromJson(data);
		if (doc.isObject()) {
			QJsonObject root = doc.object();
			for (auto it = targets_.begin(); it != targets_.end(); ++it) {
				const QString &id = it.key();
				if (root.contains(id) && root[id].isObject()) {
					QJsonObject obj = root[id].toObject();
					if (obj.contains("enabled")) {
						it->enabled = obj["enabled"].toBool();
					}
					if (obj.contains("server")) {
						QString s = obj["server"].toString().trimmed();
						if (!s.isEmpty()) {
							it->server = s;
						}
					}
					if (obj.contains("key")) {
						it->streamKey = obj["key"].toString().trimmed();
					}
					if (obj.contains("cx")) {
						it->cx = static_cast<uint32_t>(obj["cx"].toInt());
					}
					if (obj.contains("cy")) {
						it->cy = static_cast<uint32_t>(obj["cy"].toInt());
					}
				}
				const QString normalized = HKStream::ResolveTargetService(id, it->server).server;
				it->server = normalized;
			}
			blog(LOG_INFO, "[MultiStreamManager] Loaded destinations config from %s",
			     filePath.toUtf8().constData());
			return;
		}
	}

	// Fallback to OBS basic.ini / UserConfig if available
	config_t *userConfig = App() ? App()->GetUserConfig() : nullptr;
	if (userConfig) {
		for (auto it = targets_.begin(); it != targets_.end(); ++it) {
			const QString &id = it.key();
			QString sec = "MultiStream_" + id;
			if (config_has_user_value(userConfig, sec.toUtf8().constData(), "Key")) {
				it->streamKey = QString::fromUtf8(config_get_string(userConfig, sec.toUtf8().constData(), "Key"));
				it->server = QString::fromUtf8(config_get_string(userConfig, sec.toUtf8().constData(), "Server"));
				it->server = HKStream::ResolveTargetService(id, it->server).server;
				it->enabled = config_get_bool(userConfig, sec.toUtf8().constData(), "Enabled");
				if (config_has_user_value(userConfig, sec.toUtf8().constData(), "CX")) {
					it->cx = static_cast<uint32_t>(config_get_uint(userConfig, sec.toUtf8().constData(), "CX"));
					it->cy = static_cast<uint32_t>(config_get_uint(userConfig, sec.toUtf8().constData(), "CY"));
				}
			}
		}
	}
}

void MultiStreamManager::Save()
{
	QJsonObject root;
	for (auto it = targets_.begin(); it != targets_.end(); ++it) {
		QJsonObject obj;
		obj["enabled"] = it->enabled;
		obj["server"] = it->server;
		obj["key"] = it->streamKey;
		obj["cx"] = static_cast<int>(it->cx);
		obj["cy"] = static_cast<int>(it->cy);
		root[it.key()] = obj;
	}

	QJsonDocument doc(root);
	QByteArray jsonBytes = doc.toJson(QJsonDocument::Indented);

	QString mainPath = QString::fromStdString(OBSWorkspaceManager::GetWorkspaceFilePath("stream_destinations.json"));
	QStringList savePaths = { mainPath };

	QString appDir = QCoreApplication::applicationDirPath();
	QString runtimePath = appDir + "/../../workspaces/stream_destinations.json";
	if (QFileInfo(runtimePath).canonicalFilePath() != QFileInfo(mainPath).canonicalFilePath()) {
		savePaths.append(runtimePath);
	}

	for (const QString &path : savePaths) {
		QFileInfo fi(path);
		QDir dir = fi.dir();
		if (!dir.exists()) {
			if (!dir.mkpath(".")) {
				blog(LOG_ERROR, "[MultiStreamManager] Could not create destination config directory: %s",
				     QDir::toNativeSeparators(dir.absolutePath()).toUtf8().constData());
				continue;
			}
		}
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly)) {
			blog(LOG_ERROR, "[MultiStreamManager] Could not open destination config for writing: %s (%s)",
			     QDir::toNativeSeparators(path).toUtf8().constData(), file.errorString().toUtf8().constData());
			continue;
		}
		if (file.write(jsonBytes) != jsonBytes.size() || !file.commit()) {
			blog(LOG_ERROR, "[MultiStreamManager] Could not persist destination config: %s (%s)",
			     QDir::toNativeSeparators(path).toUtf8().constData(), file.errorString().toUtf8().constData());
		}
	}

	config_t *userConfig = App() ? App()->GetUserConfig() : nullptr;
	if (userConfig) {
		for (auto it = targets_.begin(); it != targets_.end(); ++it) {
			const QString &id = it.key();
			QString sec = "MultiStream_" + id;
			config_set_string(userConfig, sec.toUtf8().constData(), "Key", it->streamKey.toUtf8().constData());
			config_set_string(userConfig, sec.toUtf8().constData(), "Server", it->server.toUtf8().constData());
			config_set_bool(userConfig, sec.toUtf8().constData(), "Enabled", it->enabled);
			config_set_uint(userConfig, sec.toUtf8().constData(), "CX", it->cx);
			config_set_uint(userConfig, sec.toUtf8().constData(), "CY", it->cy);
		}
		config_save_safe(userConfig, "tmp", nullptr);
	}

	blog(LOG_INFO, "[MultiStreamManager] Saved destinations configuration");
}

QList<MultiStreamTarget> MultiStreamManager::GetAllTargets() const
{
	return targets_.values();
}

MultiStreamTarget MultiStreamManager::GetTarget(const QString &id) const
{
	return targets_.value(id);
}

void MultiStreamManager::SetTarget(const QString &id, bool enabled, const QString &server, const QString &key, uint32_t cx, uint32_t cy)
{
	if (targets_.contains(id)) {
		targets_[id].enabled = enabled;
		const QString selectedServer = server.trimmed().isEmpty() ? targets_[id].defaultServer : server.trimmed();
		targets_[id].server = HKStream::ResolveTargetService(id, selectedServer).server;
		targets_[id].streamKey = key.trimmed();
		if (cx > 0) targets_[id].cx = cx;
		if (cy > 0) targets_[id].cy = cy;
	}
}

void MultiStreamManager::SetTargetResolution(const QString &id, uint32_t cx, uint32_t cy)
{
	if (targets_.contains(id)) {
		targets_[id].cx = cx;
		targets_[id].cy = cy;
	}
}

QList<MultiStreamTarget> MultiStreamManager::GetConfiguredTargets() const
{
	QList<MultiStreamTarget> list;
	// Return in preferred display order: YouTube, Facebook, TikTok, Instagram
	QStringList order = {"youtube", "facebook", "tiktok", "instagram"};
	for (const QString &id : order) {
		if (targets_.contains(id)) {
			const MultiStreamTarget &t = targets_[id];
			if (!t.streamKey.trimmed().isEmpty() && t.enabled) {
				list.append(t);
			}
		}
	}
	return list;
}

QList<MultiStreamTarget> MultiStreamManager::GetSavedTargets() const
{
	QList<MultiStreamTarget> list;
	const QStringList order = {"youtube", "facebook", "tiktok", "instagram"};
	for (const QString &id : order) {
		if (!targets_.contains(id))
			continue;
		const MultiStreamTarget &target = targets_[id];
		if (!target.streamKey.trimmed().isEmpty())
			list.append(target);
	}
	return list;
}

void MultiStreamManager::SetActiveSessionIds(const QStringList &ids)
{
	activeSessionIds_ = ids;
}

QStringList MultiStreamManager::GetActiveSessionIds() const
{
	return activeSessionIds_;
}

OBSServiceAutoRelease MultiStreamManager::CreateServiceForTarget(const QString &id)
{
	if (!targets_.contains(id)) {
		return nullptr;
	}

	const MultiStreamTarget &t = targets_[id];
	return HKStream::CreateTargetService(id, t.server, t.streamKey, QString("multistream_svc_%1").arg(id));
}

bool MultiStreamManager::StartSecondaryForActiveSession(obs_encoder_t *videoEnc, obs_encoder_t *audioEnc,
								const std::string &workspaceId)
{
	activeWorkspaceId_ = workspaceId;
	if (activeSessionIds_.size() <= 1) {
		return true;
	}

	if (!videoEnc || !audioEnc) {
		blog(LOG_ERROR, "[MultiStreamManager] Missing video or audio encoder for secondary streams!");
		return false;
	}

	struct obs_video_info canvasOvi;
	obs_get_video_info(&canvasOvi);

	// The first target is running on the primary stream output.
	// Targets from index 1 onwards run as secondary RTMP outputs with their own resolution/mix if specified.
	for (int i = 1; i < activeSessionIds_.size(); ++i) {
		const QString &id = activeSessionIds_[i];
		if (!targets_.contains(id)) {
			continue;
		}

		MultiStreamTarget &t = targets_[id];

		// Clean up any stale output/service/encoder/view
		if (t.output) {
			if (obs_output_active(t.output)) {
				blog(LOG_WARNING, "[MultiStreamManager] Previous output is still stopping: %s",
				     t.name.toUtf8().constData());
				continue;
			}
			obs_output_release(t.output);
			t.output = nullptr;
		}
		if (t.customVideoEnc) {
			obs_encoder_release(t.customVideoEnc);
			t.customVideoEnc = nullptr;
		}
		if (t.customView) {
			obs_view_remove(t.customView);
			obs_view_set_source(t.customView, 0, nullptr);
			obs_view_destroy(t.customView);
			t.customView = nullptr;
		}
		t.customVideo = nullptr;
		if (t.service) {
			obs_service_release(t.service);
			t.service = nullptr;
		}

		// Primary and secondary destinations must use the same provider policy.
		OBSServiceAutoRelease targetService = CreateServiceForTarget(id);
		if (!targetService) {
			blog(LOG_ERROR, "[MultiStreamManager] Could not create stream service for %s", id.toUtf8().constData());
			continue;
		}
		t.service = obs_service_get_ref(targetService);

		// Create output
		t.output = obs_output_create("rtmp_output",
					     QString("sec_out_%1").arg(id).toUtf8().constData(),
					     nullptr, nullptr);

		if (!t.output) {
			blog(LOG_ERROR, "[MultiStreamManager] Failed to create rtmp_output for %s",
			     id.toUtf8().constData());
			continue;
		}

		// Check if this target needs a dedicated aspect ratio / resolution mix
		bool hasCustomRes = (t.cx > 0 && t.cy > 0) &&
				    (t.cx != canvasOvi.base_width || t.cy != canvasOvi.base_height);

		obs_encoder_t *targetVideoEnc = videoEnc;

		if (hasCustomRes) {
			// Create dedicated view and video mix
			t.customView = obs_view_create();
			obs_source_t *curScene = obs_frontend_get_current_scene();
			if (curScene) {
				std::string tAspect = OBSWorkspaceManager::GetAspectRatioKey(t.cx, t.cy);
				std::string logicalName = OBSWorkspaceManager::GetLogicalSceneName(obs_source_get_name(curScene));
				std::string backingName = OBSWorkspaceManager::GetOrCreateBackingScene(logicalName, tAspect, activeWorkspaceId_, t.cx, t.cy);
				obs_source_t *targetSceneSrc = obs_get_source_by_name(backingName.c_str());
				if (!targetSceneSrc) {
					blog(LOG_ERROR, "[MultiStreamManager] No %ux%u scene for secondary stream '%s'",
					     t.cx, t.cy, id.toUtf8().constData());
					obs_source_release(curScene);
					obs_view_destroy(t.customView);
					t.customView = nullptr;
					continue;
				}

				obs_view_set_source(t.customView, 0, targetSceneSrc);
				blog(LOG_INFO, "[MultiStreamManager] Secondary stream '%s' bound to '%s' at %ux%u",
				     id.toUtf8().constData(), obs_source_get_name(targetSceneSrc), t.cx, t.cy);
				obs_source_release(targetSceneSrc);
				obs_source_release(curScene);
			} else {
				obs_view_destroy(t.customView);
				t.customView = nullptr;
				continue;
			}

			struct obs_video_info customOvi = canvasOvi;
			customOvi.base_width = t.cx;
			customOvi.base_height = t.cy;
			customOvi.output_width = t.cx;
			customOvi.output_height = t.cy;

			t.customVideo = obs_view_add2(t.customView, &customOvi);
			if (t.customVideo) {
				const char *venc_id = videoEnc ? obs_encoder_get_id(videoEnc) : "obs_x264";
				OBSDataAutoRelease venc_settings = videoEnc ? obs_encoder_get_settings(videoEnc) : obs_data_create();
				obs_data_set_int(venc_settings, "width", t.cx);
				obs_data_set_int(venc_settings, "height", t.cy);

				t.customVideoEnc = obs_video_encoder_create(venc_id, QString("sec_venc_%1").arg(id).toUtf8().constData(), venc_settings, nullptr);
				if (!t.customVideoEnc && strcmp(venc_id, "obs_x264") != 0) {
					t.customVideoEnc = obs_video_encoder_create("obs_x264", QString("sec_venc_%1").arg(id).toUtf8().constData(), venc_settings, nullptr);
				}
				if (t.customVideoEnc) {
					obs_encoder_set_video(t.customVideoEnc, t.customVideo);
					targetVideoEnc = t.customVideoEnc;
				}
			}
		}

		obs_output_set_video_encoder(t.output, targetVideoEnc);
		obs_output_set_audio_encoder(t.output, audioEnc, 0);
		obs_output_set_service(t.output, t.service);
		obs_output_set_reconnect_settings(t.output, 5, 2);

		if (obs_output_start(t.output)) {
			t.isRunning = true;
			blog(LOG_INFO, "[MultiStreamManager] Successfully started secondary stream: %s (%s) at %ux%u",
			     t.name.toUtf8().constData(), t.server.toUtf8().constData(),
			     hasCustomRes ? t.cx : canvasOvi.base_width,
			     hasCustomRes ? t.cy : canvasOvi.base_height);
		} else {
			const char *lastErr = obs_output_get_last_error(t.output);
			blog(LOG_WARNING, "[MultiStreamManager] Failed to start secondary stream %s: %s",
			     t.name.toUtf8().constData(), lastErr ? lastErr : "Unknown error");
		}
	}

	return true;
}

void MultiStreamManager::StopAllOutputs(bool force)
{
	for (auto &target : targets_) {
		if (target.output && obs_output_active(target.output)) {
			if (force) {
				obs_output_force_stop(target.output);
			} else if (target.isRunning) {
				obs_output_stop(target.output);
			}
		}
		target.isRunning = false;
	}
	activeSessionIds_.clear();
	ReleaseStoppedOutputs(force);
}

void MultiStreamManager::ReleaseStoppedOutputs(bool force)
{
	bool pending = false;
	for (auto it = targets_.begin(); it != targets_.end(); ++it) {
		if (it->isRunning)
			continue;
		if (it->output) {
			if (!force && obs_output_active(it->output)) {
				pending = true;
				continue;
			}
			blog(LOG_INFO, "[MultiStreamManager] Released stopped stream output for %s",
			     it->name.toUtf8().constData());
			obs_output_release(it->output);
			it->output = nullptr;
		}
		if (it->customVideoEnc) {
			obs_encoder_release(it->customVideoEnc);
			it->customVideoEnc = nullptr;
		}
		if (it->customView) {
			obs_view_remove(it->customView);
			obs_view_set_source(it->customView, 0, nullptr);
			obs_view_destroy(it->customView);
			it->customView = nullptr;
		}
		it->customVideo = nullptr;
		if (it->service) {
			obs_service_release(it->service);
			it->service = nullptr;
		}
	}
	if (pending) {
		if (!stoppedOutputsTimer_.isActive())
			stoppedOutputsTimer_.start();
	} else {
		stoppedOutputsTimer_.stop();
	}
}

bool MultiStreamManager::HasActiveOutputs() const
{
	for (const auto &t : targets_) {
		// Also retain the busy state until stopped outputs and their video
		// views have been released; resetting video earlier invalidates them.
		if (t.output) {
			return true;
		}
	}
	return false;
}

void MultiStreamManager::UpdateSecondaryStreamScenes(obs_source_t *newScene)
{
	if (!newScene || !HasActiveOutputs()) return;

	for (auto &t : targets_) {
		if (t.output && obs_output_active(t.output) && t.customView && t.cx > 0 && t.cy > 0) {
			std::string tAspect = OBSWorkspaceManager::GetAspectRatioKey(t.cx, t.cy);
			std::string logicalName = OBSWorkspaceManager::GetLogicalSceneName(obs_source_get_name(newScene));
			std::string backingName = OBSWorkspaceManager::GetOrCreateBackingScene(logicalName, tAspect, activeWorkspaceId_, t.cx, t.cy);
			obs_source_t *targetSceneSrc = obs_get_source_by_name(backingName.c_str());

			if (targetSceneSrc) {
				obs_view_set_source(t.customView, 0, targetSceneSrc);
				obs_source_release(targetSceneSrc);
			} else {
				blog(LOG_ERROR, "[MultiStreamManager] No %ux%u scene for active secondary stream", t.cx, t.cy);
				obs_view_set_source(t.customView, 0, nullptr);
			}
		}
	}
}

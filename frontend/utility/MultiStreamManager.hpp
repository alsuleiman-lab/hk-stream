#pragma once

#include <obs.hpp>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QList>
#include <QObject>
#include <QTimer>

struct MultiStreamTarget {
	QString id;
	QString name;
	QString icon;
	QString defaultServer;
	QString server;
	QString streamKey;
	bool enabled = true;

	// Per-destination resolution:
	// cx = 0, cy = 0 means follow current canvas
	uint32_t cx = 0;
	uint32_t cy = 0;

	// Runtime state
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	obs_view_t *customView = nullptr;
	video_t *customVideo = nullptr;
	obs_encoder_t *customVideoEnc = nullptr;
	bool isRunning = false;
};

class MultiStreamManager : public QObject {
	Q_OBJECT

public:
	static MultiStreamManager &Instance();

	void Load();
	void Save();

	QList<MultiStreamTarget> GetAllTargets() const;
	MultiStreamTarget GetTarget(const QString &id) const;
	void SetTarget(const QString &id, bool enabled, const QString &server, const QString &key, uint32_t cx = 0, uint32_t cy = 0);
	void SetTargetResolution(const QString &id, uint32_t cx, uint32_t cy);

	// Returns targets that have non-empty stream keys
	QList<MultiStreamTarget> GetConfiguredTargets() const;
	// Returns every destination with saved credentials, including destinations disabled by default.
	QList<MultiStreamTarget> GetSavedTargets() const;

	void SetActiveSessionIds(const QStringList &ids);
	QStringList GetActiveSessionIds() const;

	OBSServiceAutoRelease CreateServiceForTarget(const QString &id);

	bool StartSecondaryForActiveSession(obs_encoder_t *videoEnc, obs_encoder_t *audioEnc,
					    const std::string &workspaceId = "");
	void StopAllOutputs(bool force = false);
	bool HasActiveOutputs() const;
	void UpdateSecondaryStreamScenes(obs_source_t *newScene);

private:
	MultiStreamManager();
	~MultiStreamManager();

	void InitDefaultTargets();
	void ReleaseStoppedOutputs(bool force = false);
	QString GetConfigFilePath() const;

	QMap<QString, MultiStreamTarget> targets_;
	QStringList activeSessionIds_;
	std::string activeWorkspaceId_;
	QTimer stoppedOutputsTimer_;
};

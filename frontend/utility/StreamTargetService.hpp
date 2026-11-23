#pragma once

#include <obs.hpp>
#include <QString>
#include <QUrl>

namespace HKStream {

inline constexpr const char *YouTubeServer = "rtmps://a.rtmps.youtube.com:443/live2";

struct TargetServiceConfig {
	QString server;
	const char *type = "rtmp_custom";
	const char *provider = nullptr;
};

inline TargetServiceConfig ResolveTargetService(const QString &id, const QString &server)
{
	TargetServiceConfig config{server.trimmed()};
	if (id != QLatin1String("youtube"))
		return config;
	if (config.server.isEmpty())
		config.server = QString::fromLatin1(YouTubeServer);

	QUrl url(config.server);
	const QString host = url.host().toLower();
	const bool primary = host == QLatin1String("a.rtmp.youtube.com") ||
			     host == QLatin1String("a.rtmps.youtube.com");
	const bool backup = host == QLatin1String("b.rtmp.youtube.com") ||
			    host == QLatin1String("b.rtmps.youtube.com");
	const bool supportedProtocol = url.scheme() == QLatin1String("rtmp") ||
				       url.scheme() == QLatin1String("rtmps");
	const bool standardPort = url.port() == -1 || url.port() == 1935 || url.port() == 443;
	if ((!primary && !backup) || !supportedProtocol || !standardPort || !url.userInfo().isEmpty() ||
	    (url.path() != QLatin1String("/live2") && url.path() != QLatin1String("/live2/")))
		return config;

	url.setScheme(QStringLiteral("rtmps"));
	url.setHost(primary ? QStringLiteral("a.rtmps.youtube.com") : QStringLiteral("b.rtmps.youtube.com"));
	url.setPort(443);
	url.setPath(QStringLiteral("/live2"));
	config.server = url.toString(QUrl::FullyEncoded);
	config.type = "rtmp_common";
	config.provider = "YouTube - RTMPS";
	return config;
}

inline OBSServiceAutoRelease CreateTargetService(const QString &id, const QString &server,
					       const QString &key, const QString &name)
{
	const TargetServiceConfig config = ResolveTargetService(id, server);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "server", config.server.toUtf8().constData());
	obs_data_set_string(settings, "key", key.trimmed().toUtf8().constData());
	if (config.provider) {
		obs_data_set_string(settings, "service", config.provider);
		obs_data_set_string(settings, "protocol", "RTMPS");
	}
	return obs_service_create(config.type, name.toUtf8().constData(), settings, nullptr);
}

} // namespace HKStream

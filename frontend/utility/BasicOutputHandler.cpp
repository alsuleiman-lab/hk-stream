#include "BasicOutputHandler.hpp"
#include "AdvancedOutput.hpp"
#include "SimpleOutput.hpp"

#include <dialogs/TitleManagerDialog.hpp>
#include <workspaces/WorkspaceModel.hpp>
#include <utility/MultitrackVideoError.hpp>
#include <utility/StartMultiTrackVideoStreamingGuard.hpp>
#include <utility/VCamConfig.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QThreadPool>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

using namespace std;

extern bool EncoderAvailable(const char *encoder);

volatile bool streaming_active = false;
volatile bool recording_active = false;
volatile bool recording_paused = false;
volatile bool replaybuf_active = false;
volatile bool virtualcam_active = false;

void OBSStreamStarting(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0) {
		return;
	}

	output->delayActive = true;
	QMetaObject::invokeMethod(output->main, &OBSBasic::StreamDelayStarting, sec);
}

void OBSStreamStopping(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0) {
		QMetaObject::invokeMethod(output->main, &OBSBasic::StreamStopping);
	} else {
		QMetaObject::invokeMethod(output->main, &OBSBasic::StreamDelayStopping, sec);
	}
}

void OBSStartStreaming(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	output->streamingActive = true;
	os_atomic_set_bool(&streaming_active, true);
	QMetaObject::invokeMethod(output->main, &OBSBasic::StreamingStart);
}

void OBSStopStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");

	QString arg_last_error = QString::fromUtf8(last_error);

	output->streamingActive = false;
	output->delayActive = false;
	output->multitrackVideoActive = false;
	os_atomic_set_bool(&streaming_active, false);
	QMetaObject::invokeMethod(output->main, &OBSBasic::StreamingStop, code, arg_last_error);
}

void OBSStartRecording(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->recordingActive = true;
	os_atomic_set_bool(&recording_active, true);
	QMetaObject::invokeMethod(output->main, &OBSBasic::RecordingStart);
}

void OBSStopRecording(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");

	QString arg_last_error = QString::fromUtf8(last_error);

	output->recordingActive = false;
	os_atomic_set_bool(&recording_active, false);
	os_atomic_set_bool(&recording_paused, false);
	QMetaObject::invokeMethod(output->main, &OBSBasic::RecordingStop, code, arg_last_error);
}

void OBSRecordStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, &OBSBasic::RecordStopping);
}

void OBSRecordFileChanged(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	const char *next_file = calldata_string(params, "next_file");

	QString arg_last_file = QString::fromUtf8(output->lastRecordingPath.c_str());

	QMetaObject::invokeMethod(output->main, &OBSBasic::RecordingFileChanged, arg_last_file);

	output->lastRecordingPath = next_file;
}

void OBSStartReplayBuffer(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->replayBufferActive = true;
	os_atomic_set_bool(&replaybuf_active, true);
	QMetaObject::invokeMethod(output->main, &OBSBasic::ReplayBufferStart);
}

void OBSStopReplayBuffer(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->replayBufferActive = false;
	os_atomic_set_bool(&replaybuf_active, false);
	QMetaObject::invokeMethod(output->main, &OBSBasic::ReplayBufferStop, code);
}

void OBSReplayBufferStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, &OBSBasic::ReplayBufferStopping);
}

void OBSReplayBufferSaved(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, &OBSBasic::ReplayBufferSaved, Qt::QueuedConnection);
}

static void OBSStartVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->virtualCamActive = true;
	os_atomic_set_bool(&virtualcam_active, true);
	QMetaObject::invokeMethod(output->main, &OBSBasic::OnVirtualCamStart);
}

static void OBSStopVirtualCam(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->virtualCamActive = false;
	os_atomic_set_bool(&virtualcam_active, false);
	QMetaObject::invokeMethod(output->main, &OBSBasic::OnVirtualCamStop, code);
}

static void OBSDeactivateVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	output->DestroyVirtualCamView();
}

bool return_first_id(void *data, const char *id)
{
	const char **output = (const char **)data;

	*output = id;
	return false;
}

const char *GetStreamOutputType(const obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	const char *output = nullptr;

	if (!protocol) {
		blog(LOG_WARNING, "The service '%s' has no protocol set", obs_service_get_id(service));
		return nullptr;
	}

	if (!obs_is_output_protocol_registered(protocol)) {
		blog(LOG_WARNING, "The protocol '%s' is not registered", protocol);
		return nullptr;
	}

	/* Check if the service has a preferred output type */
	output = obs_service_get_preferred_output_type(service);
	if (output) {
		if ((obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0) {
			return output;
		}

		blog(LOG_WARNING, "The output '%s' is not registered, fallback to another one", output);
	}

	/* Otherwise, prefer first-party output types */
	if (can_use_output(protocol, "rtmp_output", "RTMP", "RTMPS")) {
		return "rtmp_output";
	} else if (can_use_output(protocol, "ffmpeg_hls_muxer", "HLS")) {
		return "ffmpeg_hls_muxer";
	} else if (can_use_output(protocol, "ffmpeg_mpegts_muxer", "SRT", "RIST")) {
		return "ffmpeg_mpegts_muxer";
	}

	/* If third-party protocol, use the first enumerated type */
	obs_enum_output_types_with_protocol(protocol, &output, return_first_id);
	if (output) {
		return output;
	}

	blog(LOG_WARNING, "No output compatible with the service '%s' is registered", obs_service_get_id(service));

	return nullptr;
}

BasicOutputHandler::BasicOutputHandler(OBSBasic *main_) : main(main_)
{
	if (main->vcamEnabled) {
		virtualCam = obs_output_create(VIRTUAL_CAM_ID, "virtualcam_output", nullptr, nullptr);

		signal_handler_t *signal = obs_output_get_signal_handler(virtualCam);
		startVirtualCam.Connect(signal, "start", OBSStartVirtualCam, this);
		stopVirtualCam.Connect(signal, "stop", OBSStopVirtualCam, this);
		deactivateVirtualCam.Connect(signal, "deactivate", OBSDeactivateVirtualCam, this);
	}

	auto service = main_->GetService();
	OBSDataAutoRelease settings = obs_service_get_settings(service);
	auto multitrack_enabled = config_get_bool(main->Config(), "Stream1", "EnableMultitrackVideo") &&
				  (obs_data_has_user_value(settings, "multitrack_video_configuration_url") ||
				   strcmp(obs_service_get_id(service), "rtmp_custom") == 0);

	if (multitrack_enabled) {
		multitrackVideo = make_unique<MultitrackVideoOutput>();
	}

	if (config_get_int(main->Config(), "Stream1", "WHIPSimulcastTotalLayers") > 1) {
		whipSimulcastEncoders = make_unique<WHIPSimulcastEncoders>();
	}
}

extern void log_vcam_changed(const VCamConfig &config, bool starting);

namespace {
// The virtual camera is always landscape. A portrait workspace canvas must not
// turn the camera vertical; its picture is fitted inside the landscape frame.
obs_video_info GetVirtualCamVideoInfo()
{
	obs_video_info ovi{};
	obs_get_video_info(&ovi);
	if (ovi.base_height > ovi.base_width)
		std::swap(ovi.base_width, ovi.base_height);
	if (ovi.output_height > ovi.output_width)
		std::swap(ovi.output_width, ovi.output_height);
	return ovi;
}
} // namespace

bool BasicOutputHandler::StartVirtualCam()
{
	if (!main->vcamEnabled) {
		return false;
	}
	if (main->vcamConfig.type == VCamOutputType::SourceOutput) {
		OBSSourceAutoRelease selected = obs_get_source_by_name(main->vcamConfig.source.c_str());
		if (!selected || obs_source_removed(selected)) {
			struct SonySearch {
			std::vector<std::string> names;
			} search;
			obs_enum_sources(
				[](void *param, obs_source_t *candidate) {
					auto &state = *static_cast<SonySearch *>(param);
					if (obs_source_removed(candidate) || !(obs_source_get_output_flags(candidate) & OBS_SOURCE_VIDEO))
						return true;
					const char *name = obs_source_get_name(candidate);
					if (name && (QString::fromUtf8(name).contains(QStringLiteral("sony"), Qt::CaseInsensitive) ||
						     QString::fromUtf8(name).contains(QStringLiteral("سوني")))) {
						state.names.emplace_back(name);
					}
					return true;
				},
				&search);
			if (search.names.size() == 1) {
				main->vcamConfig.source = search.names.front();
				selected = obs_get_source_by_name(main->vcamConfig.source.c_str());
				main->SaveProject();
			}
		}
		if (!selected || obs_source_removed(selected)) {
			// Ask when no unique Sony source is available; never start an empty view.
			main->OpenVirtualCamConfig();
			return false;
		}
	}

	// Every output type renders through its own landscape view, so the program
	// canvas size (which follows the active workspace) never decides the shape.
	if (!virtualCamView) {
		virtualCamView = obs_view_create();
	}

	UpdateVirtualCamOutputSource();

	if (!virtualCamVideo) {
		obs_video_info ovi = GetVirtualCamVideoInfo();
		virtualCamVideo = obs_view_add2(virtualCamView, &ovi);

		if (!virtualCamVideo) {
			return false;
		}
	}

	obs_output_set_media(virtualCam, virtualCamVideo, obs_get_audio());
	if (!Active()) {
		SetupOutputs();
	}

	bool success = obs_output_start(virtualCam);
	if (!success) {
		QString errorReason;

		const char *error = obs_output_get_last_error(virtualCam);
		if (error) {
			errorReason = QT_UTF8(error);
		} else {
			errorReason = QTStr("Output.StartFailedGeneric");
		}

		QMessageBox::critical(main, QTStr("Output.StartVirtualCamFailed"), errorReason);

		DestroyVirtualCamView();
	}

	log_vcam_changed(main->vcamConfig, true);

	return success;
}

void BasicOutputHandler::StopVirtualCam()
{
	if (main->vcamEnabled) {
		obs_output_stop(virtualCam);
	}
}

bool BasicOutputHandler::VirtualCamActive() const
{
	if (main->vcamEnabled) {
		return obs_output_active(virtualCam);
	}
	return false;
}

void BasicOutputHandler::UpdateVirtualCamOutputSource()
{
	if (!main->vcamEnabled || !virtualCamView) {
		return;
	}

	OBSSourceAutoRelease content;

	switch (main->vcamConfig.type) {
	case VCamOutputType::Invalid:
	case VCamOutputType::ProgramView:
		content = obs_get_output_source(0);
		break;
	case VCamOutputType::PreviewOutput: {
		OBSSource s = main->GetCurrentSceneSource();
		content = obs_source_get_ref(s);
		break;
	}
	case VCamOutputType::SceneOutput:
		content = obs_get_source_by_name(main->vcamConfig.scene.c_str());
		break;
	case VCamOutputType::SourceOutput:
		content = obs_get_source_by_name(main->vcamConfig.source.c_str());
		break;
	}

	if (!content || obs_source_removed(content)) {
		obs_view_set_source(virtualCamView, 0, nullptr);
		DestroyVirtualCameraScene();
		return;
	}

	const obs_video_info ovi = GetVirtualCamVideoInfo();

	if (!vCamSourceScene) {
		vCamSourceScene = obs_scene_create_private("vcam_source");
	}
	obs_scene_set_custom_size(vCamSourceScene, true, ovi.base_width, ovi.base_height);

	if (vCamSourceSceneItem && (obs_sceneitem_get_source(vCamSourceSceneItem) != content)) {
		obs_sceneitem_remove(vCamSourceSceneItem);
		vCamSourceSceneItem = nullptr;
	}

	if (!vCamSourceSceneItem) {
		vCamSourceSceneItem = obs_scene_add(vCamSourceScene, content);

		obs_sceneitem_set_bounds_type(vCamSourceSceneItem, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(vCamSourceSceneItem, OBS_ALIGN_CENTER);
	}

	const struct vec2 size = {(float)ovi.base_width, (float)ovi.base_height};
	obs_sceneitem_set_bounds(vCamSourceSceneItem, &size);

	obs_source_t *source = obs_scene_get_source(vCamSourceScene);
	OBSSourceAutoRelease current = obs_view_get_source(virtualCamView, 0);
	if (source != current) {
		obs_view_set_source(virtualCamView, 0, source);
	}
}

void BasicOutputHandler::DestroyVirtualCamView()
{
	obs_view_remove(virtualCamView);
	obs_view_set_source(virtualCamView, 0, nullptr);
	virtualCamVideo = nullptr;

	obs_view_destroy(virtualCamView);
	virtualCamView = nullptr;

	DestroyVirtualCameraScene();
}

void BasicOutputHandler::DestroyVirtualCameraScene()
{
	if (!vCamSourceScene) {
		return;
	}

	obs_scene_release(vCamSourceScene);
	vCamSourceScene = nullptr;
	vCamSourceSceneItem = nullptr;
}

const char *FindAudioEncoderFromCodec(const char *type)
{
	const char *alt_enc_id = nullptr;
	size_t i = 0;

	while (obs_enum_encoder_types(i++, &alt_enc_id)) {
		const char *codec = obs_get_encoder_codec(alt_enc_id);
		if (strcmp(type, codec) == 0) {
			return alt_enc_id;
		}
	}

	return nullptr;
}

void clear_archive_encoder(obs_output_t *output, const char *expected_name)
{
	obs_encoder_t *last = obs_output_get_audio_encoder(output, 1);
	bool clear = false;

	/* ensures that we don't remove twitch's soundtrack encoder */
	if (last) {
		const char *name = obs_encoder_get_name(last);
		clear = name && strcmp(name, expected_name) == 0;
		obs_encoder_release(last);
	}

	if (clear) {
		obs_output_set_audio_encoder(output, nullptr, 1);
	}
}

void BasicOutputHandler::SetupAutoRemux(const char *&container)
{
	bool autoRemux = config_get_bool(main->Config(), "Video", "AutoRemux");
	if (autoRemux && strcmp(container, "mp4") == 0) {
		container = "mkv";
	}
}

std::string BasicOutputHandler::GetRecordingFilename(const char *path, const char *container, bool noSpace,
						     bool overwrite, const char *format, bool ffmpeg)
{
	if (!ffmpeg) {
		SetupAutoRemux(container);
	}

	string resolvedFormat = format ? format : "";
	QString activeTitle = TitleManagerDialog::GetTemporaryTitle().trimmed();
	if (activeTitle.isEmpty()) {
		activeTitle = TitleManagerDialog::GetActiveTitle().trimmed();
	}
	if (!activeTitle.isEmpty()) {
		QString safeTitle = activeTitle;
		safeTitle.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "-");
		safeTitle = safeTitle.simplified();
		// Escape single '%' to '%%' for os_generate_formatted_filename
		safeTitle.replace("%", "%%");
		if (!safeTitle.isEmpty()) {
			resolvedFormat = safeTitle.toUtf8().constData();
		}
	}

	string dst = GetOutputFilename(path, container, noSpace, overwrite, resolvedFormat.c_str());
	lastRecordingPath = dst;
	return dst;
}

extern std::string DeserializeConfigText(const char *text);

std::shared_future<void> BasicOutputHandler::SetupMultitrackVideo(obs_service_t *service, std::string audio_encoder_id,
								  size_t main_audio_mixer,
								  std::optional<size_t> vod_track_mixer,
								  std::function<void(std::optional<bool>)> continuation)
{
	auto start_streaming_guard = std::make_shared<StartMultitrackVideoStreamingGuard>();
	if (!multitrackVideo) {
		continuation(std::nullopt);
		return start_streaming_guard->GetFuture();
	}

	multitrackVideoActive = false;

	streamDelayStarting.Disconnect();
	streamStopping.Disconnect();
	startStreaming.Disconnect();
	stopStreaming.Disconnect();

	bool is_custom = strncmp("rtmp_custom", obs_service_get_type(service), 11) == 0;

	std::optional<std::string> custom_config = std::nullopt;
	if (config_get_bool(main->Config(), "Stream1", "MultitrackVideoConfigOverrideEnabled")) {
		custom_config = DeserializeConfigText(
			config_get_string(main->Config(), "Stream1", "MultitrackVideoConfigOverride"));
	}

	std::optional<QString> extraCanvasUUID;
	const char *uuid = config_get_string(main->Config(), "Stream1", "MultitrackExtraCanvas");
	if (uuid && *uuid) {
		extraCanvasUUID = uuid;
	}

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	QString key = obs_data_get_string(settings, "key");

	const char *service_name = "<unknown>";
	if (is_custom && obs_data_has_user_value(settings, "service_name")) {
		service_name = obs_data_get_string(settings, "service_name");
	} else if (!is_custom) {
		service_name = obs_data_get_string(settings, "service");
	}

	std::optional<std::string> custom_rtmp_url;
	std::optional<bool> use_rtmps;
	auto server = obs_data_get_string(settings, "server");
	if (strncmp(server, "auto", 4) != 0) {
		custom_rtmp_url = server;
	} else {
		QString server_ = server;
		use_rtmps = server_.contains("rtmps", Qt::CaseInsensitive);
	}

	auto service_custom_server = obs_data_get_bool(settings, "using_custom_server");
	if (custom_rtmp_url.has_value()) {
		blog(LOG_INFO, "Using %sserver '%s'", service_custom_server ? "custom " : "", custom_rtmp_url->c_str());
	}

	auto maximum_aggregate_bitrate =
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto")
			? std::nullopt
			: std::make_optional<uint32_t>(
				  config_get_int(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrate"));

	auto maximum_video_tracks = config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracksAuto")
					    ? std::nullopt
					    : std::make_optional<uint32_t>(config_get_int(
						      main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracks"));

	auto stream_dump_config = GenerateMultitrackVideoStreamDumpConfig();

	auto continue_on_main_thread = [&, start_streaming_guard, service = OBSService{service},
					continuation =
						std::move(continuation)](std::optional<MultitrackVideoError> error) {
		if (error) {
			OBSDataAutoRelease service_settings = obs_service_get_settings(service);
			auto multitrack_video_name = QTStr("Basic.Settings.Stream.MultitrackVideoLabel");
			if (obs_data_has_user_value(service_settings, "multitrack_video_name")) {
				multitrack_video_name = obs_data_get_string(service_settings, "multitrack_video_name");
			}

			multitrackVideoActive = false;
			if (!error->ShowDialog(main, multitrack_video_name)) {
				return continuation(false);
			}
			return continuation(std::nullopt);
		}

		multitrackVideoActive = true;

		auto signal_handler = multitrackVideo->StreamingSignalHandler();

		streamDelayStarting.Connect(signal_handler, "starting", OBSStreamStarting, this);
		streamStopping.Connect(signal_handler, "stopping", OBSStreamStopping, this);

		startStreaming.Connect(signal_handler, "start", OBSStartStreaming, this);
		stopStreaming.Connect(signal_handler, "stop", OBSStopStreaming, this);
		return continuation(true);
	};

	QThreadPool::globalInstance()->start([=, main = main, multitrackVideo = multitrackVideo.get(),
					      service_name = std::string{service_name}, service = OBSService{service},
					      stream_dump_config = OBSData{stream_dump_config},
					      start_streaming_guard = start_streaming_guard]() mutable {
		std::optional<MultitrackVideoError> error;
		try {
			multitrackVideo->PrepareStreaming(main, service_name.c_str(), service, custom_rtmp_url, key,
							  audio_encoder_id.c_str(), maximum_aggregate_bitrate,
							  maximum_video_tracks, custom_config, stream_dump_config,
							  main_audio_mixer, vod_track_mixer, use_rtmps,
							  extraCanvasUUID);
		} catch (const MultitrackVideoError &error_) {
			error.emplace(error_);
		}

		QMetaObject::invokeMethod(main, [=] { continue_on_main_thread(error); });
	});

	return start_streaming_guard->GetFuture();
}

OBSDataAutoRelease BasicOutputHandler::GenerateMultitrackVideoStreamDumpConfig()
{
	auto stream_dump_enabled = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpEnabled");

	if (!stream_dump_enabled) {
		return nullptr;
	}

	const char *path = config_get_string(main->Config(), "SimpleOutput", "FilePath");
	bool noSpace = config_get_bool(main->Config(), "SimpleOutput", "FileNameWithoutSpace");
	const char *filenameFormat = config_get_string(main->Config(), "Output", "FilenameFormatting");
	bool overwriteIfExists = config_get_bool(main->Config(), "Output", "OverwriteIfExists");
	bool useMP4 = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpAsMP4");

	string f;

	OBSDataAutoRelease settings = obs_data_create();
	f = GetFormatString(filenameFormat, nullptr, nullptr);
	string strPath = GetRecordingFilename(path, useMP4 ? "mp4" : "flv", noSpace, overwriteIfExists, f.c_str(),
					      // never remux stream dump
					      false);
	obs_data_set_string(settings, "path", strPath.c_str());

	if (useMP4) {
		obs_data_set_bool(settings, "use_mp4", true);
		obs_data_set_string(settings, "muxer_settings", "write_encoder_info=1");
	}

	return settings;
}

bool BasicOutputHandler::StartSecondaryRecording(uint32_t cx, uint32_t cy)
{
	if (secondaryRecordingActive) {
		return false;
	}

	secondaryRecordCX = cx;
	secondaryRecordCY = cy;

	// 1. Create secondary view and assign current scene source
	if (!secondaryRecordView) {
		secondaryRecordView = obs_view_create();
	}

	std::string sec_aspect = OBSWorkspaceManager::GetAspectRatioKey(cx, cy);
	OBSSource curScene = main->GetCurrentSceneSource();
	obs_source_t *targetSceneSrc = nullptr;

	if (curScene) {
		const char *curName = obs_source_get_name(curScene);
		std::string logicalName = OBSWorkspaceManager::GetLogicalSceneName(curName ? curName : "");
		const OBSWorkspace *activeWorkspace = main->workspaceManager_ ? main->workspaceManager_->GetActive() : nullptr;
		std::string workspaceId = activeWorkspace ? activeWorkspace->id : "";
		std::string backingName = OBSWorkspaceManager::GetOrCreateBackingScene(logicalName, sec_aspect, workspaceId, cx, cy);
		if (!backingName.empty())
			targetSceneSrc = obs_get_source_by_name(backingName.c_str());
	}

	if (!targetSceneSrc) {
		blog(LOG_ERROR, "[HK Stream] Cannot start secondary recording: no scene exists for %ux%u (%s)",
		     cx, cy, sec_aspect.c_str());
		DestroySecondaryRecording();
		return false;
	}

	if (targetSceneSrc) {
		obs_scene_t *sc = obs_scene_from_source(targetSceneSrc);
		if (sc) {
			obs_scene_set_custom_size(sc, true, cx, cy);
		}

		obs_view_set_source(secondaryRecordView, 0, targetSceneSrc);
		blog(LOG_INFO, "[HK Stream] Secondary recording bound to backing scene '%s' (%ux%u, aspect: %s)",
		     obs_source_get_name(targetSceneSrc), cx, cy, sec_aspect.c_str());
		obs_source_release(targetSceneSrc);
	}

	// 2. Add video mix with desired resolution
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	ovi.base_width = cx;
	ovi.base_height = cy;
	ovi.output_width = cx;
	ovi.output_height = cy;

	secondaryRecordVideo = obs_view_add2(secondaryRecordView, &ovi);
	if (!secondaryRecordVideo) {
		blog(LOG_ERROR, "[HK Stream] Failed to create secondary video mix for %ux%u", cx, cy);
		DestroySecondaryRecording();
		return false;
	}

	// 3. Create video encoder
	obs_encoder_t *primary_venc = obs_output_get_video_encoder(fileOutput);
	const char *venc_id = primary_venc ? obs_encoder_get_id(primary_venc) : "obs_x264";
	OBSDataAutoRelease venc_settings = primary_venc ? obs_encoder_get_settings(primary_venc) : obs_data_create();
	// The copied encoder settings belong to the primary canvas. Set the secondary
	// output dimensions explicitly so the encoder cannot retain the primary size.
	obs_data_set_int(venc_settings, "width", cx);
	obs_data_set_int(venc_settings, "height", cy);

	secondaryVideoEncoder = obs_video_encoder_create(venc_id, "hk_sec_video_rec", venc_settings, nullptr);
	if (!secondaryVideoEncoder && strcmp(venc_id, "obs_x264") != 0) {
		secondaryVideoEncoder = obs_video_encoder_create("obs_x264", "hk_sec_video_rec", venc_settings, nullptr);
	}
	if (!secondaryVideoEncoder) {
		blog(LOG_ERROR, "[HK Stream] Failed to create secondary video encoder");
		DestroySecondaryRecording();
		return false;
	}
	obs_encoder_set_video(secondaryVideoEncoder, secondaryRecordVideo);

	// 4. Create audio encoder
	obs_encoder_t *primary_aenc = obs_output_get_audio_encoder(fileOutput, 0);
	const char *aenc_id = primary_aenc ? obs_encoder_get_id(primary_aenc) : "ffmpeg_aac";
	OBSDataAutoRelease aenc_settings = primary_aenc ? obs_encoder_get_settings(primary_aenc) : obs_data_create();

	secondaryAudioEncoder = obs_audio_encoder_create(aenc_id, "hk_sec_audio_rec", aenc_settings, 0, nullptr);
	if (!secondaryAudioEncoder) {
		blog(LOG_ERROR, "[HK Stream] Failed to create secondary audio encoder");
		DestroySecondaryRecording();
		return false;
	}
	obs_encoder_set_audio(secondaryAudioEncoder, obs_get_audio());

	// 5. Create file output muxer
	secondaryFileOutput = obs_output_create("ffmpeg_muxer", "hk_sec_file_output", nullptr, nullptr);
	if (!secondaryFileOutput) {
		blog(LOG_ERROR, "[HK Stream] Failed to create secondary file output");
		DestroySecondaryRecording();
		return false;
	}

	obs_output_set_video_encoder(secondaryFileOutput, secondaryVideoEncoder);
	obs_output_set_audio_encoder(secondaryFileOutput, secondaryAudioEncoder, 0);

	// 6. Generate filename for secondary recording
	QString secPath;
	if (!lastRecordingPath.empty()) {
		QFileInfo fi(QString::fromUtf8(lastRecordingPath.c_str()));
		QString base = fi.completeBaseName();
		QString ext = fi.suffix();
		if (ext.isEmpty()) ext = "mp4";
		secPath = fi.dir().filePath(QString("%1_%2x%3.%4").arg(base).arg(cx).arg(cy).arg(ext));
	} else {
		const char *recPath = config_get_string(main->Config(), "SimpleOutput", "FilePath");
		if (!recPath || !*recPath) recPath = config_get_string(main->Config(), "AdvOut", "RecFilePath");
		QString dir = (recPath && *recPath) ? QString::fromUtf8(recPath) : QDir::homePath();
		QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss");
		secPath = QDir(dir).filePath(QString("%1_%2x%3.mp4").arg(timestamp).arg(cx).arg(cy));
	}
	lastSecondaryRecordingPath = secPath.toStdString();

	OBSDataAutoRelease outSettings = obs_data_create();
	obs_data_set_string(outSettings, "path", secPath.toUtf8().constData());
	obs_data_set_string(outSettings, "muxer_settings", "movflags=faststart");
	obs_output_update(secondaryFileOutput, outSettings);

	// 7. Start output
	bool success = obs_output_start(secondaryFileOutput);
	if (!success) {
		const char *err = obs_output_get_last_error(secondaryFileOutput);
		blog(LOG_ERROR, "[HK Stream] Secondary recording failed to start: %s", err ? err : "unknown error");
		DestroySecondaryRecording();
		return false;
	}

	secondaryRecordingActive = true;
	blog(LOG_INFO, "[HK Stream] Secondary recording started: %s (%ux%u)", secPath.toUtf8().constData(), cx, cy);
	return true;
}

void BasicOutputHandler::StopSecondaryRecording(bool force)
{
	if (!secondaryRecordingActive || !secondaryFileOutput) {
		return;
	}

	if (force) {
		obs_output_force_stop(secondaryFileOutput);
	} else {
		obs_output_stop(secondaryFileOutput);
	}
	secondaryRecordingActive = false;
	blog(LOG_INFO, "[HK Stream] Secondary recording stopped: %s", lastSecondaryRecordingPath.c_str());
}

void BasicOutputHandler::PauseSecondaryRecording(bool pause)
{
	if (secondaryFileOutput && secondaryRecordingActive) {
		obs_output_pause(secondaryFileOutput, pause);
	}
}

void BasicOutputHandler::DestroySecondaryRecording()
{
	if (secondaryFileOutput) {
		if (obs_output_active(secondaryFileOutput)) {
			obs_output_stop(secondaryFileOutput);
		}
		secondaryFileOutput = nullptr;
	}
	if (secondaryRecordView) {
		obs_view_remove(secondaryRecordView);
		obs_view_set_source(secondaryRecordView, 0, nullptr);
		obs_view_destroy(secondaryRecordView);
		secondaryRecordView = nullptr;
	}
	secondaryRecordVideo = nullptr;
	secondaryVideoEncoder = nullptr;
	secondaryAudioEncoder = nullptr;
	secondaryRecordingActive = false;
}

void BasicOutputHandler::UpdateSecondaryRecordingScene(obs_source_t *newScene)
{
	if (!secondaryRecordingActive || !secondaryRecordView || !newScene) {
		return;
	}

	std::string sec_aspect = OBSWorkspaceManager::GetAspectRatioKey(secondaryRecordCX, secondaryRecordCY);
	const char *curName = obs_source_get_name(newScene);
	std::string logicalName = OBSWorkspaceManager::GetLogicalSceneName(curName ? curName : "");
	const OBSWorkspace *activeWorkspace = main->workspaceManager_ ? main->workspaceManager_->GetActive() : nullptr;
	std::string workspaceId = activeWorkspace ? activeWorkspace->id : "";
		std::string backingName = OBSWorkspaceManager::GetOrCreateBackingScene(logicalName, sec_aspect, workspaceId,
																 secondaryRecordCX, secondaryRecordCY);
	obs_source_t *targetSceneSrc = backingName.empty() ? nullptr : obs_get_source_by_name(backingName.c_str());
	if (!targetSceneSrc) {
		blog(LOG_ERROR, "[HK Stream] Cannot update secondary recording: no scene exists for %ux%u (%s)",
		     secondaryRecordCX, secondaryRecordCY, sec_aspect.c_str());
		return;
	}

	if (targetSceneSrc) {
		obs_scene_t *sc = obs_scene_from_source(targetSceneSrc);
		if (sc) {
			obs_scene_set_custom_size(sc, true, secondaryRecordCX, secondaryRecordCY);
		}

		obs_view_set_source(secondaryRecordView, 0, targetSceneSrc);
		blog(LOG_INFO, "[HK Stream] Updated secondary recording scene to '%s' (aspect: %s)",
		     obs_source_get_name(targetSceneSrc), sec_aspect.c_str());
		obs_source_release(targetSceneSrc);
	}
}

BasicOutputHandler *CreateSimpleOutputHandler(OBSBasic *main)
{
	return new SimpleOutput(main);
}

BasicOutputHandler *CreateAdvancedOutputHandler(OBSBasic *main)
{
	return new AdvancedOutput(main);
}

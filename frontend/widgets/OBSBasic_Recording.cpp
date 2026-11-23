/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <components/UIValidation.hpp>
#include <dialogs/OBSRemux.hpp>
#include <dialogs/RecordingResolutionDialog.hpp>
#include <workspaces/WorkspaceModel.hpp>
#include <panels/WorkspacePanel.hpp>

#include <qt-wrappers.hpp>

#include <QDesktopServices>
#include <QFileInfo>

void OBSBasic::on_actionShow_Recordings_triggered()
{
	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	const char *type = config_get_string(activeConfiguration, "AdvOut", "RecType");
	const char *adv_path = strcmp(type, "Standard")
				       ? config_get_string(activeConfiguration, "AdvOut", "FFFilePath")
				       : config_get_string(activeConfiguration, "AdvOut", "RecFilePath");
	const char *path = strcmp(mode, "Advanced") ? config_get_string(activeConfiguration, "SimpleOutput", "FilePath")
						    : adv_path;
	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

#define RECORDING_START "==== Recording Start ==============================================="
#define RECORDING_STOP "==== Recording Stop ================================================"

extern volatile bool replaybuf_active;

void OBSBasic::AutoRemux(QString input, bool no_show)
{
	auto config = Config();

	bool autoRemux = config_get_bool(config, "Video", "AutoRemux");

	if (!autoRemux) {
		return;
	}

	bool isSimpleMode = false;

	const char *mode = config_get_string(config, "Output", "Mode");
	if (!mode) {
		isSimpleMode = true;
	} else {
		isSimpleMode = strcmp(mode, "Simple") == 0;
	}

	if (!isSimpleMode) {
		const char *recType = config_get_string(config, "AdvOut", "RecType");

		bool ffmpegOutput = astrcmpi(recType, "FFmpeg") == 0;

		if (ffmpegOutput) {
			return;
		}
	}

	if (input.isEmpty()) {
		return;
	}

	QFileInfo fi(input);
	QString suffix = fi.suffix();

	/* do not remux if lossless */
	if (suffix.compare("avi", Qt::CaseInsensitive) == 0) {
		return;
	}

	QString path = fi.path();

	QString output = input;
	output.resize(output.size() - suffix.size());

	const obs_encoder_t *videoEncoder = obs_output_get_video_encoder(outputHandler->fileOutput);
	const char *vCodecName = obs_encoder_get_codec(videoEncoder);
	const char *format = config_get_string(config, isSimpleMode ? "SimpleOutput" : "AdvOut", "RecFormat2");

	/* Retain original container for fMP4/fMOV */
	if (strncmp(format, "fragmented", 10) == 0) {
		output += "remuxed." + suffix;
	} else if (strcmp(vCodecName, "prores") == 0) {
		output += "mov";
	} else {
		output += "mp4";
	}

	OBSRemux *remux = new OBSRemux(QT_TO_UTF8(path), this, true);
	if (!no_show) {
		remux->show();
	}
	remux->AutoRemux(input, output);
}

void OBSBasic::StartRecording()
{
	if (outputHandler->RecordingActive()) {
		return;
	}
	if (disableOutputsRef) {
		return;
	}

	if (!OutputPathValid()) {
		OutputPathInvalidMessage();
		return;
	}

	if (!IsFFmpegOutputToURL() && LowDiskSpace()) {
		DiskSpaceMessage();
		return;
	}

	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STARTING);

	// If secondary recording was not explicitly prepared beforehand (e.g. started via hotkey),
	// restore the saved resolution configuration so both sources/orientations are recorded.
	if (pendingSecondaryRecordingCX_ == 0 || pendingSecondaryRecordingCY_ == 0) {
		const char *savedResStr = config_get_string(App()->GetUserConfig(), "BasicWindow", "LastSelectedRecordingResolutions");
		if (savedResStr && *savedResStr) {
			QStringList tokens = QString::fromUtf8(savedResStr).split(";", Qt::SkipEmptyParts);
			if (tokens.size() > 1) {
				auto pParts = tokens[0].split("x");
				auto sParts = tokens[1].split("x");
				if (pParts.size() == 2 && sParts.size() == 2) {
					uint32_t pCX = pParts[0].toUInt();
					uint32_t pCY = pParts[1].toUInt();
					uint32_t sCX = sParts[0].toUInt();
					uint32_t sCY = sParts[1].toUInt();

					uint32_t curCX = config_get_uint(activeConfiguration, "Video", "BaseCX");
					uint32_t curCY = config_get_uint(activeConfiguration, "Video", "BaseCY");
					if (curCX == sCX && curCY == sCY) {
						std::swap(pCX, sCX);
						std::swap(pCY, sCY);
					}

					pendingSecondaryRecordingCX_ = sCX;
					pendingSecondaryRecordingCY_ = sCY;
					ActivateWorkspaceRecordingScene(pCX, pCY);
				}
			} else if (tokens.size() == 1) {
				auto pParts = tokens[0].split("x");
				if (pParts.size() == 2) {
					ActivateWorkspaceRecordingScene(pParts[0].toUInt(), pParts[1].toUInt());
				}
			}
		} else {
			uint32_t sCX = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultSecondaryRecordingCX");
			uint32_t sCY = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultSecondaryRecordingCY");
			if (sCX > 0 && sCY > 0) {
				pendingSecondaryRecordingCX_ = sCX;
				pendingSecondaryRecordingCY_ = sCY;
			}
			uint32_t pCX = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultRecordingCX");
			uint32_t pCY = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultRecordingCY");
			if (pCX > 0 && pCY > 0) {
				ActivateWorkspaceRecordingScene(pCX, pCY);
			}
		}
	}

	SaveProjectDeferred();

	outputHandler->StartRecording();
}

void OBSBasic::RecordStopping()
{
	emit RecordingStopping();

	if (sysTrayRecord) {
		sysTrayRecord->setText(QTStr("Basic.Main.StoppingRecording"));
	}

	recordingStopping = true;
	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STOPPING);
}

void OBSBasic::StopRecording()
{
	SaveProject();

	if (outputHandler->RecordingActive()) {
		outputHandler->StopRecording(recordingStopping);
	}
	if (outputHandler->SecondaryRecordingActive()) {
		outputHandler->StopSecondaryRecording(recordingStopping);
	}

	OnDeactivate();
	RestoreStreamSessionWorkspaceVideo();
}

void OBSBasic::RecordingStart()
{
	ui->statusbar->RecordingStarted(outputHandler->fileOutput);
	emit RecordingStarted(isRecordingPausable);

	if (sysTrayRecord) {
		sysTrayRecord->setText(QTStr("Basic.Main.StopRecording"));
	}

	recordingStopping = false;
	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STARTED);

	if (pendingSecondaryRecordingCX_ > 0 && pendingSecondaryRecordingCY_ > 0 && outputHandler) {
		outputHandler->StartSecondaryRecording(pendingSecondaryRecordingCX_, pendingSecondaryRecordingCY_);
		pendingSecondaryRecordingCX_ = 0;
		pendingSecondaryRecordingCY_ = 0;
	}

	if (!diskFullTimer->isActive()) {
		diskFullTimer->start(1000);
	}

	OnActivate();

	blog(LOG_INFO, RECORDING_START);
}

void OBSBasic::RecordingStop(int code, QString last_error)
{
	ui->statusbar->RecordingStopped();
	emit RecordingStopped();

	if (sysTrayRecord) {
		sysTrayRecord->setText(QTStr("Basic.Main.StartRecording"));
	}

	blog(LOG_INFO, RECORDING_STOP);

	if (outputHandler && outputHandler->SecondaryRecordingActive()) {
		outputHandler->StopSecondaryRecording(true);
	}

	if (code == OBS_OUTPUT_UNSUPPORTED && isVisible()) {
		OBSMessageBox::critical(this, QTStr("Output.RecordFail.Title"), QTStr("Output.RecordFail.Unsupported"));

	} else if (code == OBS_OUTPUT_ENCODE_ERROR && isVisible()) {
		QString msg = last_error.isEmpty()
				      ? QTStr("Output.RecordError.EncodeErrorMsg")
				      : QTStr("Output.RecordError.EncodeErrorMsg.LastError").arg(last_error);
		OBSMessageBox::warning(this, QTStr("Output.RecordError.Title"), msg);

	} else if (code == OBS_OUTPUT_NO_SPACE && isVisible()) {
		OBSMessageBox::warning(this, QTStr("Output.RecordNoSpace.Title"), QTStr("Output.RecordNoSpace.Msg"));

	} else if (code != OBS_OUTPUT_SUCCESS && isVisible()) {

		const char *errorDescription;
		DStr errorMessage;
		bool use_last_error = true;

		errorDescription = Str("Output.RecordError.Msg");

		if (use_last_error && !last_error.isEmpty()) {
			dstr_printf(errorMessage, "%s<br><br>%s", errorDescription, QT_TO_UTF8(last_error));
		} else {
			dstr_copy(errorMessage, errorDescription);
		}

		OBSMessageBox::critical(this, QTStr("Output.RecordError.Title"), QT_UTF8(errorMessage));

	} else if (code == OBS_OUTPUT_UNSUPPORTED && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordFail.Unsupported"), QSystemTrayIcon::Warning);

	} else if (code == OBS_OUTPUT_NO_SPACE && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordNoSpace.Msg"), QSystemTrayIcon::Warning);

	} else if (code != OBS_OUTPUT_SUCCESS && !isVisible()) {
		SysTrayNotify(QTStr("Output.RecordError.Msg"), QSystemTrayIcon::Warning);
	} else if (code == OBS_OUTPUT_SUCCESS) {
		if (outputHandler) {
			std::string path = outputHandler->lastRecordingPath;
			QString str = QTStr("Basic.StatusBar.RecordingSavedTo");
			ShowStatusBarMessage(str.arg(QT_UTF8(path.c_str())));
		}
	}

	OnEvent(OBS_FRONTEND_EVENT_RECORDING_STOPPED);

	if (diskFullTimer->isActive()) {
		diskFullTimer->stop();
	}

	AutoRemux(outputHandler->lastRecordingPath.c_str());

	OnDeactivate();
	RestoreStreamSessionWorkspaceVideo();
}

void OBSBasic::RecordingFileChanged(QString lastRecordingPath)
{
	QString str = QTStr("Basic.StatusBar.RecordingSavedTo");
	ShowStatusBarMessage(str.arg(lastRecordingPath));

	AutoRemux(lastRecordingPath, true);
}

bool OBSBasic::ActivateWorkspaceRecordingScene(uint32_t cx, uint32_t cy)
{
	if (!workspaceManager_) return true;

	const OBSWorkspace *workspace = workspaceManager_->GetActive();
	if (!workspace) return true;

	if (workspace->video.base_cx != cx || workspace->video.base_cy != cy) {
		if (workspacePanel_)
			workspacePanel_->CaptureActiveSceneLayout();
		workspaceManager_->SetWorkspaceResolution(workspace->id, cx, cy);
		workspace = workspaceManager_->GetActive();
	}
	if (!workspace) return false;

	obs_video_info currentVideo{};
	if (!obs_get_video_info(&currentVideo) || currentVideo.base_width != cx || currentVideo.base_height != cy ||
	    currentVideo.output_width != workspace->video.output_cx ||
	    currentVideo.output_height != workspace->video.output_cy) {
		ApplyWorkspaceVideo(*workspace);
	}

	OBSSource currentScene = GetCurrentSceneSource();
	const char *currentSceneName = currentScene ? obs_source_get_name(currentScene) : nullptr;
	std::string logicalName = OBSWorkspaceManager::GetLogicalSceneName(
		currentSceneName ? currentSceneName : "");
	if (logicalName.empty()) {
		logicalName = !workspace->active_scene.empty()
				     ? workspace->active_scene
				     : (!workspace->scenes.empty() ? workspace->scenes.front() : "");
	}
	if (logicalName.empty()) return false;

	const std::string aspect = OBSWorkspaceManager::GetAspectRatioKey(cx, cy);
	const std::string backingName = OBSWorkspaceManager::GetOrCreateBackingScene(
		logicalName, aspect, workspace->id, cx, cy);
	obs_source_t *recordingScene = obs_get_source_by_name(backingName.c_str());
	if (recordingScene) {
		obs_frontend_set_current_scene(recordingScene);
		obs_source_release(recordingScene);
		if (workspacePanel_)
			workspacePanel_->ApplySceneLayout(aspect);
		workspaceManager_->SetActiveScene(workspace->id, logicalName);
		workspaceManager_->Save();
		blog(LOG_INFO, "[HK Stream] Recording primary scene normalized to '%s' (%ux%u)",
		     backingName.c_str(), cx, cy);
		if (workspacePanel_) workspacePanel_->RefreshAll();
		return true;
	} else {
		blog(LOG_ERROR, "[HK Stream] Could not resolve recording scene '%s'", backingName.c_str());
		return false;
	}
}

void OBSBasic::RecordActionTriggered()
{
	if (outputHandler->RecordingActive()) {
		bool confirm = config_get_bool(App()->GetUserConfig(), "BasicWindow", "WarnBeforeStoppingRecord");

		if (confirm && isVisible()) {
			QMessageBox::StandardButton button = OBSMessageBox::question(
				this, QTStr("ConfirmStopRecord.Title"), QTStr("ConfirmStopRecord.Text"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

			if (button == QMessageBox::No) {
				return;
			}
		}
		StopRecording();
	} else {
		if (!UIValidation::NoSourcesConfirmation(this)) {
			return;
		}

		if (isVisible()) {
			uint32_t curCX = config_get_uint(activeConfiguration, "Video", "BaseCX");
			uint32_t curCY = config_get_uint(activeConfiguration, "Video", "BaseCY");
			if (workspaceManager_) {
				const OBSWorkspace *activeWorkspace = workspaceManager_->GetActive();
				if (activeWorkspace) {
					curCX = activeWorkspace->video.base_cx;
					curCY = activeWorkspace->video.base_cy;
				}
			}
			if (curCX == 0) curCX = 1920;
			if (curCY == 0) curCY = 1080;

			RecordingResolutionDialog resDlg(curCX, curCY, this);
			if (resDlg.exec() != QDialog::Accepted) {
				return;
			}

			const auto &chosenList = resDlg.GetSelectedResolutions();
			uint32_t chosenCX = resDlg.GetSelectedCX();
			uint32_t chosenCY = resDlg.GetSelectedCY();

			pendingSecondaryRecordingCX_ = 0;
			pendingSecondaryRecordingCY_ = 0;

			if (chosenList.size() > 1) {
				size_t primIdx = 0;
				for (size_t i = 0; i < chosenList.size(); ++i) {
					if (chosenList[i].cx == curCX && chosenList[i].cy == curCY) {
						primIdx = i;
						break;
					}
				}
				chosenCX = chosenList[primIdx].cx;
				chosenCY = chosenList[primIdx].cy;
				size_t secIdx = (primIdx == 0) ? 1 : 0;
				pendingSecondaryRecordingCX_ = chosenList[secIdx].cx;
				pendingSecondaryRecordingCY_ = chosenList[secIdx].cy;
			}

			config_set_uint(App()->GetUserConfig(), "BasicWindow", "DefaultRecordingCX", chosenCX);
			config_set_uint(App()->GetUserConfig(), "BasicWindow", "DefaultRecordingCY", chosenCY);
			config_set_uint(App()->GetUserConfig(), "BasicWindow", "DefaultSecondaryRecordingCX", pendingSecondaryRecordingCX_);
			config_set_uint(App()->GetUserConfig(), "BasicWindow", "DefaultSecondaryRecordingCY", pendingSecondaryRecordingCY_);
			config_save_safe(App()->GetUserConfig(), "tmp", nullptr);

			if (!ActivateWorkspaceRecordingScene(chosenCX, chosenCY)) return;
		} else {
			uint32_t curCX = config_get_uint(activeConfiguration, "Video", "BaseCX");
			uint32_t curCY = config_get_uint(activeConfiguration, "Video", "BaseCY");
			if (curCX == 0) curCX = 1920;
			if (curCY == 0) curCY = 1080;

			uint32_t chosenCX = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultRecordingCX");
			uint32_t chosenCY = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultRecordingCY");
			if (chosenCX == 0 || chosenCY == 0) {
				chosenCX = curCX;
				chosenCY = curCY;
			}
			pendingSecondaryRecordingCX_ = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultSecondaryRecordingCX");
			pendingSecondaryRecordingCY_ = config_get_uint(App()->GetUserConfig(), "BasicWindow", "DefaultSecondaryRecordingCY");

			if (!ActivateWorkspaceRecordingScene(chosenCX, chosenCY)) return;
		}

		StartRecording();
	}
}

bool OBSBasic::RecordingActive()
{
	if (!outputHandler) {
		return false;
	}
	return outputHandler->RecordingActive();
}

void OBSBasic::PauseRecording()
{
	if (!outputHandler || !outputHandler->fileOutput ||
	    os_atomic_load_bool(&recording_paused)) {
		return;
	}

	obs_output_t *output = outputHandler->fileOutput;

	if (obs_output_pause(output, true)) {
		if (outputHandler && outputHandler->SecondaryRecordingActive()) {
			outputHandler->PauseSecondaryRecording(true);
		}
		os_atomic_set_bool(&recording_paused, true);

		emit RecordingPaused();

		ui->statusbar->RecordingPaused();

		TaskbarOverlaySetStatus(TaskbarOverlayStatusPaused);
		if (trayIcon && trayIcon->isVisible()) {
#ifdef __APPLE__
			QIcon trayIconFile = QIcon(":/res/images/obs_paused_macos.svg");
			trayIconFile.setIsMask(true);
#else
			QIcon trayIconFile = QIcon(":/res/images/obs_paused.png");
#endif
			trayIcon->setIcon(QIcon::fromTheme("obs-tray-paused", trayIconFile));
		}

		OnEvent(OBS_FRONTEND_EVENT_RECORDING_PAUSED);

		if (os_atomic_load_bool(&replaybuf_active)) {
			ShowReplayBufferPauseWarning();
		}
	}
}

void OBSBasic::UnpauseRecording()
{
	if (!outputHandler || !outputHandler->fileOutput ||
	    !os_atomic_load_bool(&recording_paused)) {
		return;
	}

	obs_output_t *output = outputHandler->fileOutput;

	if (obs_output_pause(output, false)) {
		if (outputHandler && outputHandler->SecondaryRecordingActive()) {
			outputHandler->PauseSecondaryRecording(false);
		}
		os_atomic_set_bool(&recording_paused, false);

		emit RecordingUnpaused();

		ui->statusbar->RecordingUnpaused();

		TaskbarOverlaySetStatus(TaskbarOverlayStatusActive);
		if (trayIcon && trayIcon->isVisible()) {
#ifdef __APPLE__
			QIcon trayIconFile = QIcon(":/res/images/tray_active_macos.svg");
			trayIconFile.setIsMask(true);
#else
			QIcon trayIconFile = QIcon(":/res/images/tray_active.png");
#endif
			trayIcon->setIcon(QIcon::fromTheme("obs-tray-active", trayIconFile));
		}

		OnEvent(OBS_FRONTEND_EVENT_RECORDING_UNPAUSED);
	}
}

void OBSBasic::RecordPauseToggled()
{
	if (!isRecordingPausable || !outputHandler || !outputHandler->fileOutput) {
		return;
	}

	obs_output_t *output = outputHandler->fileOutput;
	bool enable = !obs_output_paused(output);

	if (enable) {
		PauseRecording();
	} else {
		UnpauseRecording();
	}
}

void OBSBasic::UpdateIsRecordingPausable()
{
	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool adv = astrcmpi(mode, "Advanced") == 0;
	bool shared = true;

	if (adv) {
		const char *recType = config_get_string(activeConfiguration, "AdvOut", "RecType");

		if (astrcmpi(recType, "FFmpeg") == 0) {
			shared = config_get_bool(activeConfiguration, "AdvOut", "FFOutputToFile");
		} else {
			const char *recordEncoder = config_get_string(activeConfiguration, "AdvOut", "RecEncoder");
			shared = astrcmpi(recordEncoder, "none") == 0;
		}
	} else {
		const char *quality = config_get_string(activeConfiguration, "SimpleOutput", "RecQuality");
		shared = strcmp(quality, "Stream") == 0;
	}

	isRecordingPausable = true;
}

#define MBYTE (1024ULL * 1024ULL)
#define MBYTES_LEFT_STOP_REC 50ULL
#define MAX_BYTES_LEFT (MBYTES_LEFT_STOP_REC * MBYTE)

void OBSBasic::DiskSpaceMessage()
{
	blog(LOG_ERROR, "Recording stopped because of low disk space");

	OBSMessageBox::critical(this, QTStr("Output.RecordNoSpace.Title"), QTStr("Output.RecordNoSpace.Msg"));
}

bool OBSBasic::LowDiskSpace()
{
	const char *path;

	path = GetCurrentOutputPath();
	if (!path) {
		return false;
	}

	uint64_t num_bytes = os_get_free_disk_space(path);

	if (num_bytes < (MAX_BYTES_LEFT)) {
		return true;
	} else {
		return false;
	}
}

void OBSBasic::CheckDiskSpaceRemaining()
{
	if (LowDiskSpace()) {
		StopRecording();
		StopReplayBuffer();

		DiskSpaceMessage();
	}
}

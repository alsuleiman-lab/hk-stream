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
#include "ui-config.h"

#include "components/SourceTree.hpp"
#include "components/SourceTreeItem.hpp"
#include "ColorSelect.hpp"
#include "OBSBasicControls.hpp"
#include "OBSBasicStats.hpp"
#include "plugin-manager/PluginManager.hpp"
#include "OBSHeader.hpp"
#include "HKTitleCombo.hpp"
#include "panels/WorkspacePanel.hpp"
#include "workspaces/WorkspaceModel.hpp"
#include "dialogs/TitleManagerDialog.hpp"
#include "dialogs/SourceCategoryDialog.hpp"
#include "dialogs/WebSourceCodeDialog.hpp"

#include <obs-module.h>

#ifdef YOUTUBE_ENABLED
#include <docks/YouTubeAppDock.hpp>
#endif
#include <dialogs/NameDialog.hpp>
#include <dialogs/OBSAbout.hpp>
#include <dialogs/OBSBasicAdvAudio.hpp>
#include <dialogs/OBSBasicFilters.hpp>
#include <dialogs/OBSBasicInteraction.hpp>
#include <dialogs/OBSBasicProperties.hpp>
#include <dialogs/OBSBasicTransform.hpp>
#include <models/SceneCollection.hpp>
#include <settings/OBSBasicSettings.hpp>
#include <utility/QuickTransition.hpp>
#include <utility/SceneRenameDelegate.hpp>
#include <utility/ScreenshotObj.hpp>
#include <utility/MultiStreamManager.hpp>
#include <utility/FrameRateGuard.hpp>
#if defined(_WIN32) || defined(WHATSNEW_ENABLED)
#include <utility/WhatsNewInfoThread.hpp>
#endif
#include <widgets/AudioMixer.hpp>
#include <widgets/OBSProjector.hpp>

#include <OBSStudioAPI.hpp>
#ifdef BROWSER_AVAILABLE
#include <browser-panel.hpp>
#endif
#ifdef ENABLE_WAYLAND
#include <obs-nix-platform.h>
#endif
#include <qt-wrappers.hpp>

#include <QActionGroup>
#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QColor>
#include <QDesktopServices>
#include <QPainter>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QScrollArea>
#include <QUrl>
#include <QWidgetAction>
#include <QShortcut>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#endif

#include "moc_OBSBasic.cpp"

using namespace std;

extern bool portable_mode;
extern bool disable_3p_plugins;
extern bool opt_studio_mode;
extern bool opt_always_on_top;
extern bool opt_minimize_tray;
extern std::string opt_starting_profile;
extern std::string opt_starting_collection;

extern bool safe_mode;
extern bool opt_start_recording;
extern bool opt_start_replaybuffer;
extern bool opt_start_virtualcam;

extern volatile long insideEventLoop;
extern bool restart;

extern bool EncoderAvailable(const char *encoder);

extern void RegisterTwitchAuth();
extern void RegisterRestreamAuth();
#ifdef YOUTUBE_ENABLED
extern void RegisterYoutubeAuth();
#endif

struct QCef;

extern QCef *cef;
extern bool cef_js_avail;

extern void DestroyPanelCookieManager();
extern void CheckExistingCookieId();

namespace {
std::once_flag saveOnceFlag;

// Docks scroll their content instead of forcing a large minimum window size.
void WrapDockContentInScrollArea(QDockWidget *dock)
{
	QWidget *content = dock ? dock->widget() : nullptr;
	if (!content || qobject_cast<QScrollArea *>(content))
		return;

	auto *scroll = new QScrollArea(dock);
	scroll->setObjectName(dock->objectName() + QStringLiteral("Scroll"));
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setWidget(content);
	dock->setWidget(scroll);
}

// Starts the canvas at the active workspace's size. Otherwise loading the
// workspaces resets video right after startup, which stalls for seconds.
void ApplyActiveWorkspaceVideoConfig(config_t *config)
{
	const std::string path = OBSWorkspaceManager::GetWorkspaceFilePath("workspace.json");
	OBSDataAutoRelease state = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!state)
		return;

	const char *activeId = obs_data_get_string(state, "active_workspace");
	OBSDataArrayAutoRelease workspaces = obs_data_get_array(state, "workspaces");
	for (size_t i = 0, count = obs_data_array_count(workspaces); i < count; i++) {
		OBSDataAutoRelease workspace = obs_data_array_item(workspaces, i);
		if (strcmp(obs_data_get_string(workspace, "id"), activeId) != 0)
			continue;

		OBSDataAutoRelease video = obs_data_get_obj(workspace, "video");
		const uint64_t baseCX = obs_data_get_int(video, "base_cx");
		const uint64_t baseCY = obs_data_get_int(video, "base_cy");
		const uint64_t outputCX = obs_data_get_int(video, "output_cx");
		const uint64_t outputCY = obs_data_get_int(video, "output_cy");
		const uint64_t fpsNum = obs_data_get_int(video, "fps_num");
		const uint64_t fpsDen = obs_data_get_int(video, "fps_den");
		if (!baseCX || !baseCY || !outputCX || !outputCY || !fpsNum || !fpsDen)
			return;

		config_set_uint(config, "Video", "BaseCX", baseCX);
		config_set_uint(config, "Video", "BaseCY", baseCY);
		config_set_uint(config, "Video", "OutputCX", outputCX);
		config_set_uint(config, "Video", "OutputCY", outputCY);
		config_set_uint(config, "Video", "FPSNum", fpsNum);
		config_set_uint(config, "Video", "FPSDen", fpsDen);
		return;
	}
}

// Toolbar-like rows may be clipped when the window is narrower than their content.
void AllowShrinkBelowHint(QWidget *widget)
{
	QSizePolicy policy = widget->sizePolicy();
	policy.setHorizontalPolicy(QSizePolicy::Ignored);
	widget->setSizePolicy(policy);
	widget->setMinimumWidth(0);
}

struct WorkspaceSceneVariant {
	std::string logicalName;
	std::string workspaceId;
	uint32_t width = 0;
	uint32_t height = 0;
};

bool ParseWorkspaceSceneVariant(const std::string &name, WorkspaceSceneVariant &variant)
{
	const size_t workspaceStart = name.find("[WS:");
	const size_t sizeStart = name.find("[SIZE:");
	if (workspaceStart == std::string::npos || sizeStart == std::string::npos || sizeStart <= workspaceStart)
		return false;
	const size_t workspaceEnd = name.find(']', workspaceStart);
	const size_t sizeEnd = name.find(']', sizeStart);
	const size_t xPos = name.find('x', sizeStart + 6);
	if (workspaceEnd == std::string::npos || sizeEnd == std::string::npos || xPos == std::string::npos ||
	    xPos >= sizeEnd || xPos == sizeStart + 6 || xPos + 1 == sizeEnd)
		return false;
	try {
		variant.workspaceId = name.substr(workspaceStart + 4, workspaceEnd - workspaceStart - 4);
		variant.width = static_cast<uint32_t>(std::stoul(name.substr(sizeStart + 6, xPos - sizeStart - 6)));
		variant.height = static_cast<uint32_t>(std::stoul(name.substr(xPos + 1, sizeEnd - xPos - 1)));
	} catch (...) {
		return false;
	}
	variant.logicalName = OBSWorkspaceManager::GetLogicalSceneName(name);
	return !variant.workspaceId.empty() && variant.width > 0 && variant.height > 0;
}

struct MatchingSceneItems {
	std::string logicalSource;
	std::vector<obs_sceneitem_t *> items;
};

struct SceneItemIdentitySearch {
	obs_sceneitem_t *target = nullptr;
	bool found = false;
};

bool FindMatchingSceneItems(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	auto &search = *static_cast<MatchingSceneItems *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (name && OBSWorkspaceManager::GetLogicalSourceName(name) == search.logicalSource)
		search.items.push_back(item);
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, FindMatchingSceneItems, param);
	UNUSED_PARAMETER(scene);
	return true;
}

bool FindSceneItemIdentity(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	auto &search = *static_cast<SceneItemIdentitySearch *>(param);
	if (item == search.target) {
		search.found = true;
		return false;
	}
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, FindSceneItemIdentity, param);
	UNUSED_PARAMETER(scene);
	return !search.found;
}
}

extern void setupDockAction(QDockWidget *dock);

OBSBasic::OBSBasic(QWidget *parent) : OBSMainWindow(parent), undo_s(ui), ui(new Ui::OBSBasic)
{
	collections = {};

	setAttribute(Qt::WA_NativeWindow);

#ifdef TWITCH_ENABLED
	RegisterTwitchAuth();
#endif
#ifdef RESTREAM_ENABLED
	RegisterRestreamAuth();
#endif
#ifdef YOUTUBE_ENABLED
	RegisterYoutubeAuth();
#endif

	setAcceptDrops(true);

	setContextMenuPolicy(Qt::CustomContextMenu);

	QEvent::registerEventType(QEvent::User + QEvent::Close);

	api = InitializeAPIInterface(this);

	ui->setupUi(this);
	ui->previewDisabledWidget->setVisible(false);
	ui->autoConfigure->setVisible(false);

	/* Eliminate legacy Profiles and Scene Collections menus in favor of Workspaces System */
	if (ui->profileMenu) {
		ui->menubar->removeAction(ui->profileMenu->menuAction());
		ui->profileMenu->menuAction()->setVisible(false);
		ui->profileMenu->setEnabled(false);
	}
	if (ui->sceneCollectionMenu) {
		ui->menubar->removeAction(ui->sceneCollectionMenu->menuAction());
		ui->sceneCollectionMenu->menuAction()->setVisible(false);
		ui->sceneCollectionMenu->setEnabled(false);
	}
	if (ui->actionShowProfileFolder) {
		ui->actionShowProfileFolder->setText(QString::fromUtf8("فتح مجلد بيئات العمل (Workspaces Folder)"));
	}

	/* Initialize Smart Bounding Frame Button in Context Bar */
	sourceSmartFrameButton_ = new QPushButton(ui->contextSubContainer);
	sourceSmartFrameButton_->setObjectName("sourceSmartFrameButton");
	sourceSmartFrameButton_->setText(QString::fromUtf8("إطار ذكي"));
	sourceSmartFrameButton_->setToolTip(QString::fromUtf8("تثبيت العنصر داخل إطار محدد تلقائياً (Smart Bounding Frame)"));
	sourceSmartFrameButton_->setCheckable(true);
	sourceSmartFrameButton_->setCursor(Qt::PointingHandCursor);
	sourceSmartFrameButton_->setFixedHeight(22);

	QPixmap sfPm(16, 16);
	sfPm.fill(Qt::transparent);
	QPainter sfPainter(&sfPm);
	sfPainter.setRenderHint(QPainter::Antialiasing);
	const QColor frameColor = QApplication::palette().color(QPalette::Highlight);
	QPen framePen(frameColor);
	framePen.setWidthF(1.3);
	framePen.setStyle(Qt::DashLine);
	sfPainter.setPen(framePen);
	sfPainter.setBrush(Qt::NoBrush);
	sfPainter.drawRoundedRect(QRectF(1.5, 1.5, 13, 13), 1.5, 1.5);
	framePen.setStyle(Qt::SolidLine);
	sfPainter.setPen(framePen);
	QColor innerColor = frameColor;
	innerColor.setAlpha(105);
	sfPainter.setBrush(innerColor);
	sfPainter.drawRoundedRect(QRectF(3.5, 4, 9, 8), 1, 1);
	sourceSmartFrameButton_->setIcon(QIcon(sfPm));
	sourceSmartFrameButton_->setIconSize(QSize(14, 14));

	int lineIdx = ui->horizontalLayout_5->indexOf(ui->line);
	if (lineIdx >= 0) {
		ui->horizontalLayout_5->insertWidget(lineIdx, sourceSmartFrameButton_);
	} else {
		ui->horizontalLayout_5->addWidget(sourceSmartFrameButton_);
	}

	connect(sourceSmartFrameButton_, &QPushButton::clicked, this, [this](bool) {
		OBSSceneItem item = GetCurrentSceneItem();
		if (item && obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE)
			ToggleSmartFrame(item, OBS_BOUNDS_NONE);
		else
			ToggleSmartFrame(item, OBS_BOUNDS_SCALE_INNER);
	});

	/* Initialize Scene Burst Snapshot Button in Context Bar */
	sceneBurstSnapshotButton_ = new QPushButton(ui->contextSubContainer);
	sceneBurstSnapshotButton_->setObjectName("sceneBurstSnapshotButton");
	sceneBurstSnapshotButton_->setText(QString::fromUtf8("لقطات (3)"));
	sceneBurstSnapshotButton_->setToolTip(QString::fromUtf8("📸 التقاط 3 لقطات متتالية للمشهد (كل ثانية صورة)"));
	sceneBurstSnapshotButton_->setCursor(Qt::PointingHandCursor);
	sceneBurstSnapshotButton_->setFixedHeight(22);

	QPixmap camPm(16, 16);
	camPm.fill(Qt::transparent);
	{
		QPainter camPainter(&camPm);
		camPainter.setRenderHint(QPainter::Antialiasing);
		const QColor camColor = QApplication::palette().color(QPalette::Highlight);
		QPen camPen(camColor);
		camPen.setWidthF(1.3);
		camPainter.setPen(camPen);
		camPainter.setBrush(Qt::NoBrush);
		camPainter.drawRoundedRect(QRectF(1.2, 4.2, 13.6, 10.0), 2.0, 2.0);
		camPainter.drawRoundedRect(QRectF(5.0, 1.8, 6.0, 2.8), 1.0, 1.0);
		camPainter.drawEllipse(QPointF(8.0, 9.2), 3.0, 3.0);
		camPainter.setPen(Qt::NoPen);
		QColor dotColor = camColor;
		dotColor.setAlpha(180);
		camPainter.setBrush(dotColor);
		camPainter.drawEllipse(QPointF(12.0, 6.2), 1.0, 1.0);
	}
	sceneBurstSnapshotButton_->setIcon(QIcon(camPm));
	sceneBurstSnapshotButton_->setIconSize(QSize(14, 14));

	lineIdx = ui->horizontalLayout_5->indexOf(ui->line);
	if (lineIdx >= 0) {
		ui->horizontalLayout_5->insertWidget(lineIdx, sceneBurstSnapshotButton_);
	} else {
		ui->horizontalLayout_5->addWidget(sceneBurstSnapshotButton_);
	}

	connect(sceneBurstSnapshotButton_, &QPushButton::clicked, this, [this](bool) {
		TriggerSceneBurstSnapshots(3, 1000);
	});

	/* Set up streaming connections */
	connect(
		this, &OBSBasic::StreamingStarting, this, [this] { this->streamingStarting = true; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::StreamingStarted, this, [this] { this->streamingStarting = false; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::StreamingStopped, this, [this] { this->streamingStarting = false; },
		Qt::DirectConnection);

	/* Set up recording connections */
	connect(
		this, &OBSBasic::RecordingStarted, this,
		[this]() {
			this->recordingStarted = true;
			this->recordingPaused = false;
		},
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::RecordingPaused, this, [this]() { this->recordingPaused = true; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::RecordingUnpaused, this, [this]() { this->recordingPaused = false; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::RecordingStopped, this,
		[this]() {
			this->recordingStarted = false;
			this->recordingPaused = false;
		},
		Qt::DirectConnection);

	/* Add controls dock */
	OBSBasicControls *controls = new OBSBasicControls(this);
	controlsDock = new OBSDock(this);
	controlsDock->setObjectName(QString::fromUtf8("controlsDock"));
	controlsDock->setWindowTitle(QTStr("Basic.Main.Controls"));
	/* Parenting is done there so controls will be deleted alongside controlsDock */
	controlsDock->setWidget(controls);

	workspaceDock = new OBSDock(this);
	workspaceDock->setObjectName(QString::fromUtf8("workspaceDock"));
	workspaceDock->setWindowTitle("بيئات العمل (Workspaces)");

	connect(controls, &OBSBasicControls::StreamButtonClicked, this, &OBSBasic::StreamActionTriggered);

	connect(controls, &OBSBasicControls::StartStreamMenuActionClicked, this, &OBSBasic::StartStreaming);
	connect(controls, &OBSBasicControls::StopStreamMenuActionClicked, this, &OBSBasic::StopStreaming);
	connect(controls, &OBSBasicControls::ForceStopStreamMenuActionClicked, this, &OBSBasic::ForceStopStreaming);

	connect(controls, &OBSBasicControls::BroadcastButtonClicked, this, &OBSBasic::BroadcastButtonClicked);

	connect(controls, &OBSBasicControls::RecordButtonClicked, this, &OBSBasic::RecordActionTriggered);
	connect(controls, &OBSBasicControls::PauseRecordButtonClicked, this, &OBSBasic::RecordPauseToggled);

	connect(controls, &OBSBasicControls::ReplayBufferButtonClicked, this, &OBSBasic::ReplayBufferActionTriggered);
	connect(controls, &OBSBasicControls::SaveReplayBufferButtonClicked, this, &OBSBasic::ReplayBufferSave);

	connect(controls, &OBSBasicControls::VirtualCamButtonClicked, this, &OBSBasic::VirtualCamActionTriggered);
	connect(controls, &OBSBasicControls::VirtualCamConfigButtonClicked, this, &OBSBasic::OpenVirtualCamConfig);

	connect(controls, &OBSBasicControls::StudioModeButtonClicked, this, &OBSBasic::TogglePreviewProgramMode);

	connect(controls, &OBSBasicControls::SettingsButtonClicked, this, &OBSBasic::on_action_Settings_triggered);

	/* Set up transitions combobox connections */
	connect(this, &OBSBasic::TransitionAdded, this, [this](const QString &name, const QString &uuid) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->addItem(name, uuid);
	});
	connect(this, &OBSBasic::TransitionRenamed, this, [this](const QString &uuid, const QString &newName) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->setItemText(ui->transitions->findData(uuid), newName);
	});
	connect(this, &OBSBasic::TransitionRemoved, this, [this](const QString &uuid) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->removeItem(ui->transitions->findData(uuid));
	});
	connect(this, &OBSBasic::TransitionsCleared, this, [this]() {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->clear();
	});

	connect(this, &OBSBasic::CurrentTransitionChanged, this, [this](const QString &uuid) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->setCurrentIndex(ui->transitions->findData(uuid));
	});

	connect(ui->transitions, &QComboBox::currentIndexChanged, this,
		[this]() { SetCurrentTransition(ui->transitions->currentData().toString()); });

	connect(this, &OBSBasic::TransitionDurationChanged, this, [this](int duration) {
		QSignalBlocker sb(ui->transitionDuration);
		ui->transitionDuration->setValue(duration);
	});

	connect(ui->transitionDuration, &QSpinBox::valueChanged, this,
		[this](int value) { SetTransitionDuration(value); });

	/* Main window default layout */
	setDockCornersVertical(true);

	/* Workspace Scenes dock replaces old scenes dock */
	addDockWidget(Qt::LeftDockWidgetArea, workspaceDock);
	addDockWidget(Qt::LeftDockWidgetArea, ui->sourcesDock);
	splitDockWidget(workspaceDock, ui->sourcesDock, Qt::Vertical);
	removeDockWidget(ui->scenesDock);
	ui->scenesDock->setParent(nullptr);
	ui->scenesDock->hide();
	ui->scenesDock->toggleViewAction()->setVisible(false);
	ui->scenesDock->toggleViewAction()->setEnabled(false);
	int sideDockWidth = std::min(width() * 30 / 100, 320);
	resizeDocks({workspaceDock, ui->sourcesDock}, {sideDockWidth, sideDockWidth}, Qt::Horizontal);
	addDockWidget(Qt::BottomDockWidgetArea, controlsDock);

	startingDockLayout = saveState();

	statsDock = new OBSDock();
	statsDock->setObjectName(QStringLiteral("statsDock"));
	statsDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			       QDockWidget::DockWidgetFloatable);
	statsDock->setWindowTitle(QTStr("Basic.Stats"));
	addDockWidget(Qt::BottomDockWidgetArea, statsDock);
	statsDock->setVisible(false);
	statsDock->setFloating(true);
	statsDock->resize(700, 200);

	copyActionsDynamicProperties();

	qRegisterMetaType<int64_t>("int64_t");
	qRegisterMetaType<uint32_t>("uint32_t");
	qRegisterMetaType<OBSScene>("OBSScene");
	qRegisterMetaType<OBSSceneItem>("OBSSceneItem");
	qRegisterMetaType<OBSSource>("OBSSource");
	qRegisterMetaType<obs_hotkey_id>("obs_hotkey_id");
	qRegisterMetaType<SavedProjectorInfo *>("SavedProjectorInfo *");

	ui->scenes->setAttribute(Qt::WA_MacShowFocusRect, false);
	ui->sources->setAttribute(Qt::WA_MacShowFocusRect, false);

	bool sceneGrid = config_get_bool(App()->GetUserConfig(), "BasicWindow", "gridMode");
	ui->scenes->SetGridMode(sceneGrid);

	if (sceneGrid) {
		ui->actionSceneGridMode->setChecked(true);
	} else {
		ui->actionSceneListMode->setChecked(true);
	}

	ui->scenes->setItemDelegate(new SceneRenameDelegate(ui->scenes));

	auto displayResize = [this]() {
		struct obs_video_info ovi;

		if (obs_get_video_info(&ovi)) {
			ResizePreview(ovi.base_width, ovi.base_height);
		}

		UpdateContextBarVisibility();
		UpdatePreviewControls();
		dpi = devicePixelRatioF();

		if (isFullscreenCanvas_ && floatingCanvasCloseBtn_ && floatingCanvasCloseBtn_->isVisible()) {
			int btnX = ui->preview->width() - floatingCanvasCloseBtn_->width() - 30;
			if (btnX < 20)
				btnX = 20;
			floatingCanvasCloseBtn_->move(btnX, 25);
			floatingCanvasCloseBtn_->raise();
		}
	};
	dpi = devicePixelRatioF();

	connect(windowHandle(), &QWindow::screenChanged, this, displayResize);
	connect(ui->preview, &OBSQTDisplay::DisplayResized, this, displayResize);

	/* TODO: Move these into window-basic-preview */
	/* Preview Scaling label */
	connect(ui->preview, &OBSBasicPreview::scalingChanged, ui->previewScalePercent,
		&OBSPreviewScalingLabel::PreviewScaleChanged);

	/* Preview Scaling dropdown */
	connect(ui->preview, &OBSBasicPreview::scalingChanged, ui->previewScalingMode,
		&OBSPreviewScalingComboBox::PreviewScaleChanged);

	connect(ui->preview, &OBSBasicPreview::fixedScalingChanged, ui->previewScalingMode,
		&OBSPreviewScalingComboBox::PreviewFixedScalingChanged);

	connect(ui->previewScalingMode, &OBSPreviewScalingComboBox::currentIndexChanged, this,
		&OBSBasic::PreviewScalingModeChanged);

	/* Fullscreen Canvas */
	if (ui->previewFullscreenButton) {
		ui->previewFullscreenButton->setObjectName("previewFullscreenButton");
		connect(ui->previewFullscreenButton, &QPushButton::clicked, this, &OBSBasic::ToggleFullscreenCanvas);
	}

	QShortcut *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
	connect(escShortcut, &QShortcut::activated, this, [this]() {
		if (isFullscreenCanvas_) {
			ExitFullscreenCanvas();
		}
	});

	QShortcut *f11Shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
	connect(f11Shortcut, &QShortcut::activated, this, &OBSBasic::ToggleFullscreenCanvas);

	/* Preview Controls */
	connect(ui->previewXScrollBar, &QScrollBar::sliderMoved, ui->preview, &OBSBasicPreview::xScrollBarChanged);
	connect(ui->previewYScrollBar, &QScrollBar::valueChanged, ui->preview, &OBSBasicPreview::yScrollBarChanged);

	connect(ui->previewZoomInButton, &QPushButton::clicked, ui->preview, &OBSBasicPreview::increaseScalingLevel);
	connect(ui->previewZoomOutButton, &QPushButton::clicked, ui->preview, &OBSBasicPreview::decreaseScalingLevel);

	/* Preview Actions */
	connect(ui->actionScaleWindow, &QAction::triggered, this, &OBSBasic::setPreviewScalingWindow);
	connect(ui->actionScaleCanvas, &QAction::triggered, this, &OBSBasic::setPreviewScalingCanvas);
	connect(ui->actionScaleOutput, &QAction::triggered, this, &OBSBasic::setPreviewScalingOutput);

	connect(ui->actionPreviewZoomIn, &QAction::triggered, ui->preview, &OBSBasicPreview::increaseScalingLevel);
	connect(ui->actionPreviewZoomOut, &QAction::triggered, ui->preview, &OBSBasicPreview::decreaseScalingLevel);
	connect(ui->actionPreviewResetZoom, &QAction::triggered, ui->preview, &OBSBasicPreview::resetScalingLevel);

	connect(this, &OBSBasic::CanvasResized, ui->previewScalingMode, &OBSPreviewScalingComboBox::CanvasResized);
	connect(this, &OBSBasic::OutputResized, ui->previewScalingMode, &OBSPreviewScalingComboBox::OutputResized);

	delete shortcutFilter;
	shortcutFilter = CreateShortcutFilter();
	installEventFilter(shortcutFilter);

	stringstream name;
	name << "OBS " << App()->GetVersionString();
	blog(LOG_INFO, "%s", name.str().c_str());
	blog(LOG_INFO, "---------------------------------");

	UpdateTitleBar();

	connect(ui->scenes->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, &OBSBasic::SceneNameEdited);

	cpuUsageInfo = os_cpu_usage_info_start();
	cpuUsageTimer = new QTimer(this);
	connect(cpuUsageTimer.data(), &QTimer::timeout, ui->statusbar, &OBSBasicStatusBar::UpdateCPUUsage);
	cpuUsageTimer->start(3000);

	diskFullTimer = new QTimer(this);
	connect(diskFullTimer, &QTimer::timeout, this, &OBSBasic::CheckDiskSpaceRemaining);

	renameScene = new QAction(QTStr("Rename"), ui->scenesDock);
	renameScene->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameScene, &QAction::triggered, this, &OBSBasic::EditSceneName);
	ui->scenesDock->addAction(renameScene);

	renameSource = new QAction(QTStr("Rename"), ui->sourcesDock);
	renameSource->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameSource, &QAction::triggered, this, &OBSBasic::EditSceneItemName);
	ui->sourcesDock->addAction(renameSource);

#ifdef __APPLE__
	renameScene->setShortcut({Qt::Key_Return});
	renameSource->setShortcut({Qt::Key_Return});

	ui->actionRemoveSource->setShortcuts({Qt::Key_Backspace, Qt::Key_Delete});
	ui->actionRemoveScene->setShortcuts({Qt::Key_Backspace, Qt::Key_Delete});

	ui->actionCheckForUpdates->setMenuRole(QAction::AboutQtRole);
	ui->action_Settings->setMenuRole(QAction::PreferencesRole);
	ui->actionShowMacPermissions->setMenuRole(QAction::ApplicationSpecificRole);
	delete ui->actionE_xit;
#else
	renameScene->setShortcut({Qt::Key_F2});
	renameSource->setShortcut({Qt::Key_F2});
#endif

#ifdef __linux__
	ui->actionE_xit->setShortcut(Qt::CTRL | Qt::Key_Q);
#endif

#ifndef ENABLE_IDIAN_PLAYGROUND
	ui->idianPlayground->setVisible(false);
#endif

	auto addNudge = [this](const QKeySequence &seq, MoveDir direction, int distance) {
		QAction *nudge = new QAction(ui->preview);
		nudge->setShortcut(seq);
		nudge->setShortcutContext(Qt::WidgetShortcut);
		ui->preview->addAction(nudge);
		connect(nudge, &QAction::triggered, this,
			[this, distance, direction]() { Nudge(distance, direction); });
	};

	addNudge(Qt::Key_Up, MoveDir::Up, 1);
	addNudge(Qt::Key_Down, MoveDir::Down, 1);
	addNudge(Qt::Key_Left, MoveDir::Left, 1);
	addNudge(Qt::Key_Right, MoveDir::Right, 1);
	addNudge(Qt::SHIFT | Qt::Key_Up, MoveDir::Up, 10);
	addNudge(Qt::SHIFT | Qt::Key_Down, MoveDir::Down, 10);
	addNudge(Qt::SHIFT | Qt::Key_Left, MoveDir::Left, 10);
	addNudge(Qt::SHIFT | Qt::Key_Right, MoveDir::Right, 10);

	/* Setup dock toggle action
	 * And hide all docks before restoring parent geometry */
#define SETUP_DOCK(dock)                                    \
	setupDockAction(dock);                              \
	ui->menuDocks->addAction(dock->toggleViewAction()); \
	dock->setVisible(false);

	// ui->scenesDock is superseded by workspaceDock; do not add to menuDocks
	setupDockAction(ui->scenesDock);
	ui->scenesDock->setVisible(false);
	ui->scenesDock->toggleViewAction()->setVisible(false);
	SETUP_DOCK(ui->sourcesDock);
	SETUP_DOCK(ui->mixerDock);
	SETUP_DOCK(ui->transitionsDock);
	SETUP_DOCK(controlsDock);
	SETUP_DOCK(statsDock);
	SETUP_DOCK(workspaceDock);
#undef SETUP_DOCK

	// Register shortcuts for Undo/Redo
	ui->actionMainUndo->setShortcut(Qt::CTRL | Qt::Key_Z);
	QList<QKeySequence> shrt;
	shrt << QKeySequence((Qt::CTRL | Qt::SHIFT) | Qt::Key_Z) << QKeySequence(Qt::CTRL | Qt::Key_Y);
	ui->actionMainRedo->setShortcuts(shrt);

	ui->actionMainUndo->setShortcutContext(Qt::ApplicationShortcut);
	ui->actionMainRedo->setShortcutContext(Qt::ApplicationShortcut);

	QPoint curPos;

	//restore parent window geometry
	const char *geometry = config_get_string(App()->GetUserConfig(), "BasicWindow", "geometry");
	if (geometry != NULL) {
		QByteArray byteArray = QByteArray::fromBase64(QByteArray(geometry));
		restoreGeometry(byteArray);

		QRect windowGeometry = normalGeometry();
		if (!WindowPositionValid(windowGeometry)) {
			QRect rect = QGuiApplication::primaryScreen()->geometry();
			setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size(), rect));
		}

		curPos = pos();
	} else {
		QRect desktopRect = QGuiApplication::primaryScreen()->geometry();
		QSize adjSize = desktopRect.size() / 2 - size() / 2;
		curPos = QPoint(adjSize.width(), adjSize.height());
	}

	QPoint curSize(width(), height());

	QPoint statsDockSize(statsDock->width(), statsDock->height());
	QPoint statsDockPos = curSize / 2 - statsDockSize / 2;
	QPoint newPos = curPos + statsDockPos;
	statsDock->move(newPos);

	ui->actionReleaseNotes->setVisible(true);

	ui->previewDisabledWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui->enablePreviewButton, &QPushButton::clicked, this, &OBSBasic::TogglePreview);

	connect(ui->scenes, &SceneTree::scenesReordered, ui->scenes,
		[]() { OBSProjector::UpdateMultiviewProjectors(); });

	connect(App(), &OBSApp::StyleChanged, this, [this]() { OnEvent(OBS_FRONTEND_EVENT_THEME_CHANGED); });
#ifndef __APPLE__
	connect(App(), &OBSApp::aboutToQuit, this, &OBSBasic::closeWindow);
#endif

	QActionGroup *actionGroup = new QActionGroup(this);
	actionGroup->addAction(ui->actionSceneListMode);
	actionGroup->addAction(ui->actionSceneGridMode);

	UpdatePreviewSafeAreas();
	UpdatePreviewSpacingHelpers();
	UpdatePreviewOverflowSettings();
}

static const double scaled_vals[] = {1.0, 1.25, (1.0 / 0.75), 1.5, (1.0 / 0.6), 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 0.0};

#define DEFAULT_CONTAINER "mp4"

bool OBSBasic::InitBasicConfigDefaults()
{
	QList<QScreen *> screens = QGuiApplication::screens();

	if (!screens.size()) {
		OBSErrorBox(NULL, "There appears to be no monitors.  Er, this "
				  "technically shouldn't be possible.");
		return false;
	}

	QScreen *primaryScreen = QGuiApplication::primaryScreen();

	uint32_t cx = primaryScreen->size().width();
	uint32_t cy = primaryScreen->size().height();

	cx *= devicePixelRatioF();
	cy *= devicePixelRatioF();

	bool oldResolutionDefaults = config_get_bool(App()->GetUserConfig(), "General", "Pre19Defaults");

	/* use 1920x1080 for new default base res if main monitor is above
	 * 1920x1080, but don't apply for people from older builds -- only to
	 * new users */
	if (!oldResolutionDefaults && (cx * cy) > (1920 * 1080)) {
		cx = 1920;
		cy = 1080;
	}

	bool changed = false;

	/* ----------------------------------------------------- */
	/* move over old FFmpeg track settings                   */
	if (config_has_user_value(activeConfiguration, "AdvOut", "FFAudioTrack") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "Pre22.1Settings")) {

		int track = (int)config_get_int(activeConfiguration, "AdvOut", "FFAudioTrack");
		config_set_int(activeConfiguration, "AdvOut", "FFAudioMixes", 1LL << (track - 1));
		config_set_bool(activeConfiguration, "AdvOut", "Pre22.1Settings", true);
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* move over mixer values in advanced if older config */
	if (config_has_user_value(activeConfiguration, "AdvOut", "RecTrackIndex") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "RecTracks")) {

		uint64_t track = config_get_uint(activeConfiguration, "AdvOut", "RecTrackIndex");
		track = 1ULL << (track - 1);
		config_set_uint(activeConfiguration, "AdvOut", "RecTracks", track);
		config_remove_value(activeConfiguration, "AdvOut", "RecTrackIndex");
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* set twitch chat extensions to "both" if prev version  */
	/* is under 24.1                                         */
	if (config_get_bool(App()->GetUserConfig(), "General", "Pre24.1Defaults") &&
	    !config_has_user_value(activeConfiguration, "Twitch", "AddonChoice")) {
		config_set_int(activeConfiguration, "Twitch", "AddonChoice", 3);
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* move bitrate enforcement setting to new value         */
	if (config_has_user_value(activeConfiguration, "SimpleOutput", "EnforceBitrate") &&
	    !config_has_user_value(activeConfiguration, "Stream1", "IgnoreRecommended") &&
	    !config_has_user_value(activeConfiguration, "Stream1", "MovedOldEnforce")) {
		bool enforce = config_get_bool(activeConfiguration, "SimpleOutput", "EnforceBitrate");
		config_set_bool(activeConfiguration, "Stream1", "IgnoreRecommended", !enforce);
		config_set_bool(activeConfiguration, "Stream1", "MovedOldEnforce", true);
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* enforce minimum retry delay of 1 second prior to 27.1 */
	if (config_has_user_value(activeConfiguration, "Output", "RetryDelay")) {
		int retryDelay = config_get_uint(activeConfiguration, "Output", "RetryDelay");
		if (retryDelay < 1) {
			config_set_uint(activeConfiguration, "Output", "RetryDelay", 1);
			changed = true;
		}
	}

	/* ----------------------------------------------------- */
	/* Migrate old container selection (if any) to new key.  */

	auto MigrateFormat = [&](const char *section) {
		bool has_old_key = config_has_user_value(activeConfiguration, section, "RecFormat");
		bool has_new_key = config_has_user_value(activeConfiguration, section, "RecFormat2");
		if (!has_new_key && !has_old_key) {
			return;
		}

		string old_format =
			config_get_string(activeConfiguration, section, has_new_key ? "RecFormat2" : "RecFormat");
		string new_format = old_format;
		if (old_format == "ts") {
			new_format = "mpegts";
		} else if (old_format == "m3u8") {
			new_format = "hls";
		} else if (old_format == "fmp4") {
			new_format = "fragmented_mp4";
		} else if (old_format == "fmov") {
			new_format = "fragmented_mov";
		}

		if (new_format != old_format || !has_new_key) {
			config_set_string(activeConfiguration, section, "RecFormat2", new_format.c_str());
			changed = true;
		}
	};

	MigrateFormat("AdvOut");
	MigrateFormat("SimpleOutput");

	/* ----------------------------------------------------- */
	/* Migrate output scale setting to GPU scaling options.  */

	if (config_get_bool(activeConfiguration, "AdvOut", "Rescale") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "RescaleFilter")) {
		config_set_int(activeConfiguration, "AdvOut", "RescaleFilter", OBS_SCALE_BILINEAR);
	}

	if (config_get_bool(activeConfiguration, "AdvOut", "RecRescale") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "RecRescaleFilter")) {
		config_set_int(activeConfiguration, "AdvOut", "RecRescaleFilter", OBS_SCALE_BILINEAR);
	}

	/* ----------------------------------------------------- */

	if (changed) {
		activeConfiguration.SaveSafe("tmp");
	}

	/* ----------------------------------------------------- */

	config_set_default_string(activeConfiguration, "Output", "Mode", "Simple");

	config_set_default_bool(activeConfiguration, "Stream1", "IgnoreRecommended", false);
	config_set_default_bool(activeConfiguration, "Stream1", "EnableMultitrackVideo", false);
	config_set_default_bool(activeConfiguration, "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto", true);
	config_set_default_bool(activeConfiguration, "Stream1", "MultitrackVideoMaximumVideoTracksAuto", true);

	config_set_default_string(activeConfiguration, "SimpleOutput", "FilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecFormat2", DEFAULT_CONTAINER);
	config_set_default_uint(activeConfiguration, "SimpleOutput", "VBitrate", 6000);
	config_set_default_uint(activeConfiguration, "SimpleOutput", "ABitrate", 160);
	config_set_default_bool(activeConfiguration, "SimpleOutput", "UseAdvanced", false);
	config_set_default_string(activeConfiguration, "SimpleOutput", "Preset", "veryfast");
	config_set_default_string(activeConfiguration, "SimpleOutput", "NVENCPreset2", "p5");
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecQuality", "Stream");
	config_set_default_bool(activeConfiguration, "SimpleOutput", "RecRB", false);
	config_set_default_int(activeConfiguration, "SimpleOutput", "RecRBTime", 20);
	config_set_default_int(activeConfiguration, "SimpleOutput", "RecRBSize", 512);
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecRBPrefix", "Replay");
	config_set_default_string(activeConfiguration, "SimpleOutput", "StreamAudioEncoder", "aac");
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecAudioEncoder", "aac");
	config_set_default_uint(activeConfiguration, "SimpleOutput", "RecTracks", (1 << 0));

	config_set_default_bool(activeConfiguration, "AdvOut", "ApplyServiceSettings", true);
	config_set_default_bool(activeConfiguration, "AdvOut", "UseRescale", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "TrackIndex", 1);
	config_set_default_uint(activeConfiguration, "AdvOut", "VodTrackIndex", 2);
	config_set_default_string(activeConfiguration, "AdvOut", "Encoder", "obs_x264");

	config_set_default_string(activeConfiguration, "AdvOut", "RecType", "Standard");

	config_set_default_string(activeConfiguration, "AdvOut", "RecFilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(activeConfiguration, "AdvOut", "RecFormat2", DEFAULT_CONTAINER);
	config_set_default_bool(activeConfiguration, "AdvOut", "RecUseRescale", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "RecTracks", (1 << 0));
	config_set_default_string(activeConfiguration, "AdvOut", "RecEncoder", "none");
	config_set_default_uint(activeConfiguration, "AdvOut", "FLVTrack", 1);
	config_set_default_uint(activeConfiguration, "AdvOut", "StreamMultiTrackAudioMixes", 1);
	config_set_default_bool(activeConfiguration, "AdvOut", "FFOutputToFile", true);
	config_set_default_string(activeConfiguration, "AdvOut", "FFFilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(activeConfiguration, "AdvOut", "FFExtension", "mp4");
	config_set_default_uint(activeConfiguration, "AdvOut", "FFVBitrate", 6000);
	config_set_default_uint(activeConfiguration, "AdvOut", "FFVGOPSize", 250);
	config_set_default_bool(activeConfiguration, "AdvOut", "FFUseRescale", false);
	config_set_default_bool(activeConfiguration, "AdvOut", "FFIgnoreCompat", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "FFABitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "FFAudioMixes", 1);

	config_set_default_uint(activeConfiguration, "AdvOut", "Track1Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track2Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track3Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track4Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track5Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track6Bitrate", 160);

	config_set_default_uint(activeConfiguration, "AdvOut", "RecSplitFileTime", 15);
	config_set_default_uint(activeConfiguration, "AdvOut", "RecSplitFileSize", 2048);

	config_set_default_bool(activeConfiguration, "AdvOut", "RecRB", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "RecRBTime", 20);
	config_set_default_int(activeConfiguration, "AdvOut", "RecRBSize", 512);

	config_set_default_uint(activeConfiguration, "Video", "BaseCX", cx);
	config_set_default_uint(activeConfiguration, "Video", "BaseCY", cy);

	/* don't allow BaseCX/BaseCY to be susceptible to defaults changing */
	if (!config_has_user_value(activeConfiguration, "Video", "BaseCX") ||
	    !config_has_user_value(activeConfiguration, "Video", "BaseCY")) {
		config_set_uint(activeConfiguration, "Video", "BaseCX", cx);
		config_set_uint(activeConfiguration, "Video", "BaseCY", cy);
		config_save_safe(activeConfiguration, "tmp", nullptr);
	}

	config_set_default_string(activeConfiguration, "Output", "FilenameFormatting", "%CCYY-%MM-%DD %hh-%mm-%ss");

	config_set_default_bool(activeConfiguration, "Output", "DelayEnable", false);
	config_set_default_uint(activeConfiguration, "Output", "DelaySec", 20);
	config_set_default_bool(activeConfiguration, "Output", "DelayPreserve", true);

	config_set_default_bool(activeConfiguration, "Output", "Reconnect", true);
	config_set_default_uint(activeConfiguration, "Output", "RetryDelay", 2);
	config_set_default_uint(activeConfiguration, "Output", "MaxRetries", 25);

	config_set_default_string(activeConfiguration, "Output", "BindIP", "default");
	config_set_default_string(activeConfiguration, "Output", "IPFamily", "IPv4+IPv6");
	config_set_default_bool(activeConfiguration, "Output", "NewSocketLoopEnable", false);
	config_set_default_bool(activeConfiguration, "Output", "LowLatencyEnable", false);

	int i = 0;
	uint32_t scale_cx = cx;
	uint32_t scale_cy = cy;

	/* use a default scaled resolution that has a pixel count no higher
	 * than 1280x720 */
	while (((scale_cx * scale_cy) > (1280 * 720)) && scaled_vals[i] > 0.0) {
		double scale = scaled_vals[i++];
		scale_cx = uint32_t(double(cx) / scale);
		scale_cy = uint32_t(double(cy) / scale);
	}

	config_set_default_uint(activeConfiguration, "Video", "OutputCX", scale_cx);
	config_set_default_uint(activeConfiguration, "Video", "OutputCY", scale_cy);

	/* don't allow OutputCX/OutputCY to be susceptible to defaults
	 * changing */
	if (!config_has_user_value(activeConfiguration, "Video", "OutputCX") ||
	    !config_has_user_value(activeConfiguration, "Video", "OutputCY")) {
		config_set_uint(activeConfiguration, "Video", "OutputCX", scale_cx);
		config_set_uint(activeConfiguration, "Video", "OutputCY", scale_cy);
		config_save_safe(activeConfiguration, "tmp", nullptr);
	}

	config_set_default_uint(activeConfiguration, "Video", "FPSType", 0);
	config_set_default_string(activeConfiguration, "Video", "FPSCommon", "30");
	config_set_default_uint(activeConfiguration, "Video", "FPSInt", 30);
	config_set_default_uint(activeConfiguration, "Video", "FPSNum", 30);
	config_set_default_uint(activeConfiguration, "Video", "FPSDen", 1);
	config_set_default_string(activeConfiguration, "Video", "ScaleType", "bicubic");
	config_set_default_string(activeConfiguration, "Video", "ColorFormat", "NV12");
	config_set_default_string(activeConfiguration, "Video", "ColorSpace", "709");
	config_set_default_string(activeConfiguration, "Video", "ColorRange", "Partial");
	config_set_default_uint(activeConfiguration, "Video", "SdrWhiteLevel", 300);
	config_set_default_uint(activeConfiguration, "Video", "HdrNominalPeakLevel", 1000);

	config_set_default_string(activeConfiguration, "Audio", "MonitoringDeviceId", "default");
	config_set_default_string(activeConfiguration, "Audio", "MonitoringDeviceName",
				  Str("Basic.Settings.Advanced.Audio.MonitoringDevice"
				      ".Default"));
	config_set_default_uint(activeConfiguration, "Audio", "SampleRate", 48000);
	config_set_default_string(activeConfiguration, "Audio", "ChannelSetup", "Stereo");
	config_set_default_double(activeConfiguration, "Audio", "MeterDecayRate", VOLUME_METER_DECAY_FAST);
	config_set_default_uint(activeConfiguration, "Audio", "PeakMeterType", 0);

	CheckExistingCookieId();

	return true;
}

void OBSBasic::InitBasicConfigDefaults2()
{
	bool oldEncDefaults = config_get_bool(App()->GetUserConfig(), "General", "Pre23Defaults");
	bool useNV = EncoderAvailable("ffmpeg_nvenc") && !oldEncDefaults;

	config_set_default_string(activeConfiguration, "SimpleOutput", "StreamEncoder",
				  useNV ? SIMPLE_ENCODER_NVENC : SIMPLE_ENCODER_X264);
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecEncoder",
				  useNV ? SIMPLE_ENCODER_NVENC : SIMPLE_ENCODER_X264);

	const char *aac_default = "ffmpeg_aac";
	if (EncoderAvailable("CoreAudio_AAC")) {
		aac_default = "CoreAudio_AAC";
	} else if (EncoderAvailable("libfdk_aac")) {
		aac_default = "libfdk_aac";
	}

	config_set_default_string(activeConfiguration, "AdvOut", "AudioEncoder", aac_default);
	config_set_default_string(activeConfiguration, "AdvOut", "RecAudioEncoder", aac_default);
}

bool OBSBasic::InitBasicConfig()
{
	ProfileScope("OBSBasic::InitBasicConfig");

	RefreshProfiles(true);

	const std::string currentProfileName{config_get_string(App()->GetUserConfig(), "Basic", "Profile")};
	const std::optional<OBSProfile> currentProfile = GetProfileByName(currentProfileName);
	const std::optional<OBSProfile> foundProfile = GetProfileByName(opt_starting_profile);

	try {
		if (foundProfile) {
			ActivateProfile(foundProfile.value());
		} else if (currentProfile) {
			ActivateProfile(currentProfile.value());
		} else {
			const OBSProfile &newProfile = CreateProfile(currentProfileName);
			ActivateProfile(newProfile);
		}
	} catch (const std::logic_error &) {
		OBSErrorBox(NULL, "Failed to open basic.ini: %d", -1);
		return false;
	}

	return true;
}

void OBSBasic::InitOBSCallbacks()
{
	ProfileScope("OBSBasic::InitOBSCallbacks");

	signalHandlers.reserve(signalHandlers.size() + 6);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_create", OBSBasic::SourceCreated, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_remove", OBSBasic::SourceRemoved, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_rename", OBSBasic::SourceRenamed, this);
	signalHandlers.emplace_back(
		obs_get_signal_handler(), "source_filter_add",
		[](void *data, calldata_t *) {
			QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::UpdateEditMenu,
						  Qt::QueuedConnection);
		},
		this);
	signalHandlers.emplace_back(
		obs_get_signal_handler(), "source_filter_remove",
		[](void *data, calldata_t *) {
			QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::UpdateEditMenu,
						  Qt::QueuedConnection);
		},
		this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "canvas_remove", OBSBasic::CanvasRemoved, this);
}

#define STARTUP_SEPARATOR "==== Startup complete ==============================================="
#define SHUTDOWN_SEPARATOR "==== Shutting down =================================================="

#define UNSUPPORTED_ERROR                                                     \
	"Failed to initialize video:\n\nRequired graphics API functionality " \
	"not found.  Your GPU may not be supported."

#define UNKNOWN_ERROR                                                  \
	"Failed to initialize video.  Your GPU may not be supported, " \
	"or your graphics drivers may need to be updated."

static inline void LogEncoders()
{
	constexpr uint32_t hide_flags = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL;

	auto list_encoders = [](obs_encoder_type type) {
		size_t idx = 0;
		const char *encoder_type;

		while (obs_enum_encoder_types(idx++, &encoder_type)) {
			if (obs_get_encoder_caps(encoder_type) & hide_flags ||
			    obs_get_encoder_type(encoder_type) != type) {
				continue;
			}

			blog(LOG_INFO, "\t- %s (%s)", encoder_type, obs_encoder_get_display_name(encoder_type));
		}
	};

	blog(LOG_INFO, "---------------------------------");
	blog(LOG_INFO, "Available Encoders:");
	blog(LOG_INFO, "  Video Encoders:");
	list_encoders(OBS_ENCODER_VIDEO);
	blog(LOG_INFO, "  Audio Encoders:");
	list_encoders(OBS_ENCODER_AUDIO);
}

void OBSBasic::OBSInit()
{
	ProfileScope("OBSBasic::OBSInit");

	if (!InitBasicConfig()) {
		throw "Failed to load basic.ini";
	}
	if (!ResetAudio()) {
		throw "Failed to initialize audio";
	}

	int ret = 0;

	ApplyActiveWorkspaceVideoConfig(activeConfiguration);
	ret = ResetVideo();

	switch (ret) {
	case OBS_VIDEO_MODULE_NOT_FOUND:
		throw "Failed to initialize video:  Graphics module not found";
	case OBS_VIDEO_NOT_SUPPORTED:
		throw UNSUPPORTED_ERROR;
	case OBS_VIDEO_INVALID_PARAM:
		throw "Failed to initialize video:  Invalid parameters";
	default:
		if (ret != OBS_VIDEO_SUCCESS) {
			throw UNKNOWN_ERROR;
		}
	}

	/* load audio monitoring */
	if (obs_audio_monitoring_available()) {
		const char *device_name = config_get_string(activeConfiguration, "Audio", "MonitoringDeviceName");
		const char *device_id = config_get_string(activeConfiguration, "Audio", "MonitoringDeviceId");

		obs_set_audio_monitoring_device(device_name, device_id);

		blog(LOG_INFO, "Audio monitoring device:\n\tname: %s\n\tid: %s", device_name, device_id);
	}

	InitOBSCallbacks();
	InitHotkeys();
	ui->preview->Init();

	/* hack to prevent elgato from loading its own QtNetwork that it tries
	 * to ship with */
#if defined(_WIN32) && !defined(_DEBUG)
	LoadLibraryW(L"Qt6Network");
#endif
	// Modules can access frontend information (i.e., profile and scene collection data) during their initialization,
	// and some modules (e.g., obs-websockets) are known to use the filesystem location of the current profile in their
	// own code.
	//
	// Thus, the profile and scene collection discovery needs to happen before any access to that information happens,
	// but after initializing global settings, to ensure legacy code gets valid path information.
	RefreshSceneCollections(true);

	App()->loadAppModules();

#ifdef BROWSER_AVAILABLE
	cef = obs_browser_init_panel();
	cef_js_avail = cef && obs_browser_qcef_version() >= 3;
#endif

	vcamEnabled = (obs_get_output_flags(VIRTUAL_CAM_ID) & OBS_OUTPUT_VIDEO) != 0;
	if (vcamEnabled) {
		emit VirtualCamEnabled();
	}

	UpdateProfileEncoders();

	LogEncoders();

	blog(LOG_INFO, STARTUP_SEPARATOR);

	if (!InitService()) {
		throw "Failed to initialize service";
	}

	ResetOutputs();
	CreateHotkeys();

	InitPrimitives();

	sceneDuplicationMode = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SceneDuplicationMode");
	swapScenesMode = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SwapScenesMode");
	editPropertiesMode = config_get_bool(App()->GetUserConfig(), "BasicWindow", "EditPropertiesMode");

	if (!opt_studio_mode) {
		SetPreviewProgramMode(config_get_bool(App()->GetUserConfig(), "BasicWindow", "PreviewProgramMode"));
	} else {
		SetPreviewProgramMode(true);
		opt_studio_mode = false;
	}

#define SET_VISIBILITY(name, control)                                                                \
	do {                                                                                         \
		if (config_has_user_value(App()->GetUserConfig(), "BasicWindow", name)) {            \
			bool visible = config_get_bool(App()->GetUserConfig(), "BasicWindow", name); \
			ui->control->setChecked(visible);                                            \
		}                                                                                    \
	} while (false)

	SET_VISIBILITY("ShowListboxToolbars", toggleListboxToolbars);
	SET_VISIBILITY("ShowStatusBar", toggleStatusBar);
#undef SET_VISIBILITY

	bool sourceIconsVisible = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowSourceIcons");
	ui->toggleSourceIcons->setChecked(sourceIconsVisible);

	bool contextVisible = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowContextToolbars");
	ui->toggleContextBar->setChecked(contextVisible);
	ui->contextContainer->setVisible(contextVisible);
	if (contextVisible) {
		UpdateContextBar(true);
	}
	UpdateEditMenu();

	{
		ProfileScope("OBSBasic::Load");
		const std::string sceneCollectionName{
			config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};
		std::optional<OBS::SceneCollection> configuredCollection =
			GetSceneCollectionByName(sceneCollectionName);
		std::optional<OBS::SceneCollection> foundCollection = GetSceneCollectionByName(opt_starting_collection);

		if (foundCollection) {
			ActivateSceneCollection(foundCollection.value());
		} else if (configuredCollection) {
			ActivateSceneCollection(configuredCollection.value());
		} else {
			disableSaving--;
			SetupNewSceneCollection(sceneCollectionName);
			disableSaving++;
		}

		if (foundCollection || configuredCollection) {
			ProfileScope("OBSBasic::Load collection events");
			disableSaving--;
			OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
			OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED);
			OnEvent(OBS_FRONTEND_EVENT_SCENE_CHANGED);
			OnEvent(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
			disableSaving++;
		}
	}

	loaded = true;

	previewEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "PreviewEnabled");

	if (!previewEnabled && !IsPreviewProgramMode()) {
		QMetaObject::invokeMethod(this, &OBSBasic::EnablePreviewDisplay, Qt::QueuedConnection, previewEnabled);
	} else if (!previewEnabled && IsPreviewProgramMode()) {
		QMetaObject::invokeMethod(this, &OBSBasic::EnablePreviewDisplay, Qt::QueuedConnection, true);
	}

	disableSaving--;

	auto addDisplay = [this](OBSQTDisplay *window) {
		obs_display_add_draw_callback(window->GetDisplay(), OBSBasic::RenderMain, this);

		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			ResizePreview(ovi.base_width, ovi.base_height);
		}
	};

	connect(ui->preview, &OBSQTDisplay::DisplayCreated, this, addDisplay);

	/* Initialize Workspaces */
	workspaceManager_ = std::make_unique<OBSWorkspaceManager>();
	std::string wsPath = OBSWorkspaceManager::GetWorkspaceFilePath("workspace.json");
	const OBS::SceneCollection &activeCollection = GetCurrentSceneCollection();
	{
		ProfileScope("OBSWorkspaceManager::Load (init)");
		workspaceManager_->Load(wsPath, activeCollection.getFilePathString(),
					 OBSWorkspaceManager::GetProjectFilePath(activeCollection.getFileName()));
	}
	const OBSWorkspace *activeWs = workspaceManager_->GetActive();
	if (activeWs) {
		ProfileScope("OBSBasic::ApplyWorkspaceVideo (init)");
		ApplyWorkspaceVideo(*activeWs);
	}

	/* Initialize and insert OBSHeader directly above the Video Canvas */
	obsHeader_ = new OBSHeader(this, ui->centralwidget);
	ui->verticalLayout->insertWidget(0, obsHeader_);

	connect(obsHeader_, &OBSHeader::streamClicked, this, &OBSBasic::StreamActionTriggered);
	connect(obsHeader_, &OBSHeader::recordClicked, this, &OBSBasic::RecordActionTriggered);
	connect(obsHeader_, &OBSHeader::vcamClicked, this, &OBSBasic::VirtualCamActionTriggered);
	connect(this, &OBSBasic::StreamingStarted, obsHeader_, &OBSHeader::onStreamStarted);
	connect(this, &OBSBasic::StreamingStopped, obsHeader_, &OBSHeader::onStreamStopped);
	connect(this, &OBSBasic::RecordingStarted, obsHeader_, &OBSHeader::onRecordStarted);
	connect(this, &OBSBasic::RecordingStopped, obsHeader_, &OBSHeader::onRecordStopped);
	connect(this, &OBSBasic::RecordingPaused,  obsHeader_, &OBSHeader::onRecordPaused);
	connect(this, &OBSBasic::RecordingUnpaused,obsHeader_, &OBSHeader::onRecordUnpaused);
	connect(this, &OBSBasic::VirtualCamStarted, obsHeader_, &OBSHeader::onVCamStarted);
	connect(this, &OBSBasic::VirtualCamStopped, obsHeader_, &OBSHeader::onVCamStopped);

	/* Source filter (all / core / variable / audio) directly inside the Sources Dock */
	auto *sourceTabWidget = new QWidget(ui->sourcesFrame);
	sourceTabWidget->setLayoutDirection(Qt::RightToLeft);
	auto *stLayout = new QVBoxLayout(sourceTabWidget);
	stLayout->setContentsMargins(2, 2, 2, 2);
	stLayout->setSpacing(2);

	sourceLockNotice_ = new QLabel(sourceTabWidget);
	sourceLockNotice_->setObjectName("sourceLockNotice");
	sourceLockNotice_->setProperty("noticeMode", "variable");
	sourceLockNotice_->setVisible(false);
	stLayout->addWidget(sourceLockNotice_);

	sourceCategoryFilter_ = new QComboBox(sourceTabWidget);
	sourceCategoryFilter_->setObjectName("sourceCategoryFilter");
	sourceCategoryFilter_->setLayoutDirection(Qt::RightToLeft);
	sourceCategoryFilter_->addItem("📋 الشامل", 0);
	sourceCategoryFilter_->addItem("🛡️ الأساسية", 1);
	sourceCategoryFilter_->addItem("⚡ المتغيرة", 2);
	sourceCategoryFilter_->addItem("🎙️ الأصوات", 3);
	sourceCategoryFilter_->setMinimumContentsLength(10);
	sourceCategoryFilter_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	sourceCategoryFilter_->setCursor(Qt::PointingHandCursor);
	// Always open in the variable-source view.
	sourceCategoryFilter_->setCurrentIndex(2);

	auto *tabRow = new QHBoxLayout();
	tabRow->setContentsMargins(0, 0, 0, 0);
	tabRow->setSpacing(4);
	tabRow->addWidget(sourceCategoryFilter_, 1);

	auto *btnManageCategories = new QPushButton("⚙️ تصنيف", sourceTabWidget);
	btnManageCategories->setObjectName("btnManageCategories");
	btnManageCategories->setFixedHeight(28);
	btnManageCategories->setToolTip("فتح محرر تصنيف المصادر الأساسية والمتغيرة");
	btnManageCategories->setCursor(Qt::PointingHandCursor);
	connect(btnManageCategories, &QPushButton::clicked, this, &OBSBasic::OpenSourceCategoryDialog);
	tabRow->addWidget(btnManageCategories);

	stLayout->addLayout(tabRow);

	// Keep title selection beside the top output controls; the editor lives in its popup.
	auto *titleSelector = new HKTitleCombo(obsHeader_);
	titleQuickCombo_ = titleSelector;
	titleQuickCombo_->setObjectName("titleQuickCombo");
	titleQuickCombo_->setLayoutDirection(Qt::RightToLeft);
	titleQuickCombo_->setCursor(Qt::PointingHandCursor);
	obsHeader_->addTitleSelector(titleQuickCombo_);
	txtTempTitle_ = titleSelector->temporaryEdit;
	btnApplyTemp_ = titleSelector->applyTemporary;
	btnCancelTemp_ = titleSelector->restorePermanent;

	auto applyTemp = [this, titleSelector]() {
		const QString text = txtTempTitle_->text().trimmed();
		if (text.isEmpty()) return;
		TitleManagerDialog::ApplyTemporaryTitleToOBS(text);
		UpdateTitleQuickCombo();
		titleSelector->hidePopup();
	};
	connect(btnApplyTemp_, &QPushButton::clicked, this, applyTemp);
	connect(txtTempTitle_, &QLineEdit::returnPressed, this, applyTemp);
	connect(btnCancelTemp_, &QPushButton::clicked, this, [this, titleSelector]() {
		TitleManagerDialog::RestoreTemporaryTitleFromOBS();
		txtTempTitle_->clear();
		UpdateTitleQuickCombo();
		titleSelector->hidePopup();
	});

	if (obsHeader_) {
		connect(obsHeader_, &OBSHeader::lessonNumberChanged, this, [this](int num) {
			TitleManagerDialog::UpdateLessonNumberForActive(num);
			UpdateTitleQuickCombo();
		});
	}

	{
		ProfileScope("OBSBasic::UpdateTitleQuickCombo");
		UpdateTitleQuickCombo();
	}
	{
		ProfileScope("TitleManagerDialog::ApplyTitleToOBS (init)");
		const QString activeTitle = TitleManagerDialog::GetActiveTitle().trimmed();
		const auto entries = TitleManagerDialog::LoadTitlesData();
		for (const auto &entry : entries) {
			if (entry.text.trimmed() == activeTitle) {
				TitleManagerDialog::ApplyTitleToOBS(entry.text, entry.fontFace, entry.fontSize, entry.isBold);
				break;
			}
		}
	}
	connect(titleQuickCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
		if (index < 0) return;
		QString selected = titleQuickCombo_->itemData(index).toString();
		if (selected.isEmpty()) {
			selected = titleQuickCombo_->itemText(index);
		}
		selected = selected.trimmed();
		if (selected.isEmpty()) return;

		TitleManagerDialog::RestoreTemporaryTitleFromOBS();
		const auto entries = TitleManagerDialog::LoadTitlesData();
		bool applied = false;
		for (const auto &entry : entries) {
			if (entry.text.trimmed() == selected) {
				TitleManagerDialog::ApplyTitleToOBS(entry.text, entry.fontFace, entry.fontSize, entry.isBold);
				TitleManagerDialog::SaveTitlesData(entries, entry.text);
				applied = true;
				break;
			}
		}
		if (!applied) {
			TitleManagerDialog::ApplyTitleToOBS(selected);
		}
		if (txtTempTitle_) txtTempTitle_->clear();
		UpdateTitleQuickCombo();
	});

	ui->verticalLayout_17->insertWidget(0, sourceTabWidget);

	connect(sourceCategoryFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
		FilterSourcesByCategory(idx);
	});
	// Scene changes reset/repopulate the source model without changing the
	// category combo. Reapply the active filter after the new rows are in place,
	// otherwise sources from the other category briefly (or indefinitely) show.
	if (QAbstractItemModel *sourceModel = ui->sources->model()) {
		auto refreshCategoryFilter = [this]() {
			QTimer::singleShot(0, this, [this]() {
				if (sourceCategoryFilter_)
					FilterSourcesByCategory(sourceCategoryFilter_->currentIndex());
			});
		};
		connect(sourceModel, &QAbstractItemModel::modelReset, this, refreshCategoryFilter);
		connect(sourceModel, &QAbstractItemModel::rowsInserted, this,
			[this, refreshCategoryFilter](const QModelIndex &, int, int) { refreshCategoryFilter(); });
		connect(sourceModel, &QAbstractItemModel::rowsRemoved, this,
			[this, refreshCategoryFilter](const QModelIndex &, int, int) { refreshCategoryFilter(); });
	}

	/* Initialize Workspace Panel */
	{
		ProfileScope("WorkspacePanel::WorkspacePanel");
		workspacePanel_ = new WorkspacePanel(workspaceManager_.get(), this, workspaceDock);
	}
	workspaceDock->setWidget(workspacePanel_);
	workspaceDock->setVisible(true);

	connect(workspacePanel_, &WorkspacePanel::workspaceChanged, this, [this](const QString &) {
		if (const OBSWorkspace *active = workspaceManager_->GetActive()) {
			ApplyWorkspaceVideo(*active);
		}
		UpdateWorkspaceMenu();
		UpdateTitleBar();
	});

	connect(this, &OBSBasic::StreamingStarted, this, [this]() {
		if (sourceCategoryFilter_ && sourceCategoryFilter_->currentIndex() == 1) {
			sourceCategoryFilter_->setCurrentIndex(2);
		}
		if (sourceCategoryFilter_) {
			FilterSourcesByCategory(sourceCategoryFilter_->currentIndex());
		}
	});

	connect(this, &OBSBasic::StreamingStopped, this, [this]() {
		if (sourceCategoryFilter_) {
			FilterSourcesByCategory(sourceCategoryFilter_->currentIndex());
		}
	});

	if (const OBSWorkspace *active = workspaceManager_->GetActive()) {
		ProfileScope("OBSBasic::ApplyWorkspaceVideo (panel)");
		ApplyWorkspaceVideo(*active);
	}

	/* Log repeated canvas pictures while preserving the user's selected recording rate. */
	new FrameRateGuard(this);

	/* Setup and populate Workspaces Menu in the top menubar */
	SetupWorkspaceMenu();


	/* Show the main window, unless the tray icon isn't available
	 * or neither the setting nor flag for starting minimized is set. */
	bool sysTrayEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayEnabled");
	bool sysTrayWhenStarted = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayWhenStarted");
	bool hideWindowOnStart = QSystemTrayIcon::isSystemTrayAvailable() && sysTrayEnabled &&
				 (opt_minimize_tray || sysTrayWhenStarted);

#ifdef _WIN32
	SetWin32DropStyle(this);

	if (!hideWindowOnStart) {
		show();
	}
#endif

	bool alwaysOnTop = config_get_bool(App()->GetUserConfig(), "BasicWindow", "AlwaysOnTop");

#ifdef ENABLE_WAYLAND
	bool isWayland = obs_get_nix_platform() == OBS_NIX_PLATFORM_WAYLAND;
#else
	bool isWayland = false;
#endif

	if (!isWayland && (alwaysOnTop || opt_always_on_top)) {
		SetAlwaysOnTop(this, true);
		ui->actionAlwaysOnTop->setChecked(true);
	} else if (isWayland) {
		if (opt_always_on_top) {
			blog(LOG_INFO, "Always On Top not available on Wayland, ignoring.");
		}
		ui->actionAlwaysOnTop->setEnabled(false);
		ui->actionAlwaysOnTop->setVisible(false);
	}

#ifndef _WIN32
	if (!hideWindowOnStart) {
		show();
	}
#endif

	// Set up Audio Mixer dock
	AudioMixer *audioMixer = new AudioMixer(this);
	ui->mixerDock->setWidget(audioMixer);
	ui->mixerDock->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(ui->mixerDock, &QDockWidget::customContextMenuRequested, audioMixer, &AudioMixer::showMixerContextMenu);

	/* let the main window shrink well below its docks' natural size */
	WrapDockContentInScrollArea(ui->mixerDock);
	WrapDockContentInScrollArea(controlsDock);
	WrapDockContentInScrollArea(workspaceDock);
	AllowShrinkBelowHint(ui->contextContainer);
	AllowShrinkBelowHint(ui->previewXContainer);
	AllowShrinkBelowHint(ui->statusbar);

	/* setup stats dock */
	OBSBasicStats *statsDlg = new OBSBasicStats(statsDock, false);
	statsDock->setWidget(statsDlg);

	/* ----------------------------- */
	/* add custom browser docks      */
#if defined(BROWSER_AVAILABLE) && defined(YOUTUBE_ENABLED)
	YouTubeAppDock::CleanupYouTubeUrls();
#endif

#ifdef BROWSER_AVAILABLE
	if (cef) {
		QAction *action = new QAction(QTStr("Basic.MainMenu.Docks."
						    "CustomBrowserDocks"),
					      this);
		ui->menuDocks->insertAction(ui->scenesDock->toggleViewAction(), action);
		connect(action, &QAction::triggered, this, &OBSBasic::ManageExtraBrowserDocks);
		ui->menuDocks->insertSeparator(ui->scenesDock->toggleViewAction());

		LoadExtraBrowserDocks();
	}
#endif

#ifdef YOUTUBE_ENABLED
	/* setup YouTube app dock */
	if (YouTubeAppDock::IsYTServiceSelected()) {
		NewYouTubeAppDock();
	}
#endif

	const char *dockStateStr = config_get_string(App()->GetUserConfig(), "BasicWindow", "DockState");

	if (!dockStateStr) {
		on_resetDocks_triggered(true);
	} else {
		QByteArray dockState = QByteArray::fromBase64(QByteArray(dockStateStr));
		if (!restoreState(dockState)) {
			dockStateRestoreFailed_ = true;
			blog(LOG_WARNING,
			     "[Docks] Saved dock layout could not be restored. Keeping the current layout and preserving the saved state for recovery.");
		}
	}

#ifdef BROWSER_AVAILABLE
	FinalizeExtraBrowserDockObjectNames();
#endif

	dockStateSaveTimer_ = new QTimer(this);
	dockStateSaveTimer_->setSingleShot(true);
	dockStateSaveTimer_->setInterval(750);
	connect(dockStateSaveTimer_, &QTimer::timeout, this, &OBSBasic::SaveDockState);
	for (QDockWidget *dock : findChildren<QDockWidget *>())
		TrackDockForPersistence(dock);
	connect(this, &QMainWindow::tabifiedDockWidgetActivated, this,
		[this](QDockWidget *) { ScheduleDockStateSave(); });
	QTimer::singleShot(0, this, [this]() {
		ClampFloatingDockWidgets();
		QTimer::singleShot(750, this, [this]() {
			if (dockStateRestoreFailed_)
				lastSavedDockState_ = saveState();
			dockStatePersistenceReady_ = true;
			SaveDockState();
		});
	});

	bool pre23Defaults = config_get_bool(App()->GetUserConfig(), "General", "Pre23Defaults");
	if (pre23Defaults) {
		bool resetDockLock23 = config_get_bool(App()->GetUserConfig(), "General", "ResetDockLock23");
		if (!resetDockLock23) {
			config_set_bool(App()->GetUserConfig(), "General", "ResetDockLock23", true);
			config_remove_value(App()->GetUserConfig(), "BasicWindow", "DocksLocked");
			config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
		}
	}

	bool docksLocked = config_get_bool(App()->GetUserConfig(), "BasicWindow", "DocksLocked");
	on_lockDocks_toggled(docksLocked);
	ui->lockDocks->blockSignals(true);
	ui->lockDocks->setChecked(docksLocked);
	ui->lockDocks->blockSignals(false);

	bool sideDocks = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks");
	ui->sideDocks->setChecked(sideDocks);
	setDockCornersVertical(sideDocks);

	SystemTray(true);

	TaskbarOverlayInit();

#ifdef __APPLE__
	disableColorSpaceConversion(this);
#endif

	bool has_last_version = config_has_user_value(App()->GetAppConfig(), "General", "LastVersion");
	bool first_run = config_get_bool(App()->GetUserConfig(), "General", "FirstRun");

	if (!first_run) {
		config_set_bool(App()->GetUserConfig(), "General", "FirstRun", true);
		config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	}

	// AutoConfigure wizard disabled
	// if (!first_run && !has_last_version && !Active()) {
	// 	QMetaObject::invokeMethod(this, &OBSBasic::on_autoConfigure_triggered, Qt::QueuedConnection);
	// }

#if (defined(_WIN32) || defined(__APPLE__)) && (OBS_RELEASE_CANDIDATE > 0 || OBS_BETA > 0)
	/* Automatically set branch to "beta" the first time a pre-release build is run. */
	if (!config_get_bool(App()->GetAppConfig(), "General", "AutoBetaOptIn")) {
		config_set_string(App()->GetAppConfig(), "General", "UpdateBranch", "beta");
		config_set_bool(App()->GetAppConfig(), "General", "AutoBetaOptIn", true);
		config_save_safe(App()->GetAppConfig(), "tmp", nullptr);
	}
#endif
	TimedCheckForUpdates();

	emit userSettingChanged("BasicWindow", "VerticalVolumeControl");

	if (config_get_bool(activeConfiguration, "General", "OpenStatsOnStartup")) {
		on_stats_triggered();
	}

	OBSBasicStats::InitializeValues();

	/* ----------------------- */
	/* Add multiview menu      */

	ui->viewMenu->addSeparator();

	connect(ui->viewMenu->menuAction(), &QAction::hovered, this, &OBSBasic::updateMultiviewProjectorMenu);
	OBSBasic::updateMultiviewProjectorMenu();

	ui->sources->UpdateIcons();

#if !defined(_WIN32)
	delete ui->actionRepair;
	ui->actionRepair = nullptr;
#if !defined(__APPLE__)
	delete ui->actionShowCrashLogs;
	delete ui->actionUploadLastCrashLog;
	delete ui->menuCrashLogs;
	delete ui->actionCheckForUpdates;
	ui->actionShowCrashLogs = nullptr;
	ui->actionUploadLastCrashLog = nullptr;
	ui->menuCrashLogs = nullptr;
	ui->actionCheckForUpdates = nullptr;
#endif
#endif

#ifdef __APPLE__
	/* Remove OBS' Fullscreen Interface menu in favor of the one macOS adds by default */
	delete ui->actionFullscreenInterface;
	ui->actionFullscreenInterface = nullptr;
#else
	/* Don't show menu to raise macOS-only permissions dialog */
	delete ui->actionShowMacPermissions;
	ui->actionShowMacPermissions = nullptr;
#endif

#if defined(_WIN32) || defined(__APPLE__)
	if (App()->IsUpdaterDisabled()) {
		ui->actionCheckForUpdates->setEnabled(false);
#if defined(_WIN32)
		ui->actionRepair->setEnabled(false);
#endif
	}
#endif

#ifndef WHATSNEW_ENABLED
	delete ui->actionShowWhatsNew;
	ui->actionShowWhatsNew = nullptr;
#endif

	if (safe_mode) {
		ui->actionRestartSafe->setText(QTStr("Basic.MainMenu.Help.RestartNormal"));
	}

	UpdatePreviewProgramIndicators();
	OnFirstLoad();

	if (!hideWindowOnStart) {
		activateWindow();
	}

	App()->CloseStartupSplash();

	App()->handlePluginLoadState();

}

void OBSBasic::OnFirstLoad()
{
	OnEvent(OBS_FRONTEND_EVENT_FINISHED_LOADING);

#ifdef WHATSNEW_ENABLED
	/* Attempt to load init screen if available */
	if (cef) {
		WhatsNewInfoThread *wnit = new WhatsNewInfoThread();
		connect(wnit, &WhatsNewInfoThread::Result, this, &OBSBasic::ReceivedIntroJson, Qt::QueuedConnection);

		introCheckThread.reset(wnit);
		introCheckThread->start();
	}
#endif

	Auth::Load();

	bool showLogViewerOnStartup = config_get_bool(App()->GetUserConfig(), "LogViewer", "ShowLogStartup");

	if (showLogViewerOnStartup) {
		on_actionViewCurrentLog_triggered();
	}
}

OBSBasic::~OBSBasic() {}

void OBSBasic::applicationShutdown() noexcept
{
	/* clear out UI event queue */
	QApplication::sendPostedEvents(nullptr);
#ifndef __APPLE__
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
#endif

	if (updateCheckThread && updateCheckThread->isRunning()) {
		updateCheckThread->wait();
	}

	if (patronJsonThread && patronJsonThread->isRunning()) {
		patronJsonThread->wait();
	}

	delete screenshotData;
	delete previewProjectorSource;
	delete previewProjectorMain;
	delete sourceProjector;
	delete sceneProjectorMenu;
	delete colorMenu;
	delete colorWidgetAction;
	delete colorSelect;
	delete deinterlaceMenu;
	delete perSceneTransitionMenu;
	delete shortcutFilter;
	delete programOptions;
	delete program;

	/* XXX: any obs data must be released before calling obs_shutdown.
	 * currently, we can't automate this with C++ RAII because of the
	 * delicate nature of obs_shutdown needing to be freed before the UI
	 * can be freed, and we have no control over the destruction order of
	 * the Qt UI stuff, so we have to manually clear any references to
	 * libobs. */
	delete cpuUsageTimer;
	os_cpu_usage_info_destroy(cpuUsageInfo);

	obs_hotkey_set_callback_routing_func(nullptr, nullptr);
	ClearHotkeys();

	service = nullptr;
	outputHandler.reset();

	delete interaction;
	delete properties;
	delete filters;
	delete transformWindow;
	delete advAudioWindow;
	delete about;
	delete remux;

	obs_display_remove_draw_callback(ui->preview->GetDisplay(), OBSBasic::RenderMain, this);

	obs_enter_graphics();
	gs_vertexbuffer_destroy(box);
	gs_vertexbuffer_destroy(boxLeft);
	gs_vertexbuffer_destroy(boxTop);
	gs_vertexbuffer_destroy(boxRight);
	gs_vertexbuffer_destroy(boxBottom);
	gs_vertexbuffer_destroy(circle);
	gs_vertexbuffer_destroy(actionSafeMargin);
	gs_vertexbuffer_destroy(graphicsSafeMargin);
	gs_vertexbuffer_destroy(fourByThreeSafeMargin);
	gs_vertexbuffer_destroy(leftLine);
	gs_vertexbuffer_destroy(topLine);
	gs_vertexbuffer_destroy(rightLine);
	obs_leave_graphics();

	/* When shutting down, sometimes source references can get in to the
	 * event queue, and if we don't forcibly process those events they
	 * won't get processed until after obs_shutdown has been called.  I
	 * really wish there were a more elegant way to deal with this via C++,
	 * but Qt doesn't use C++ in a normal way, so you can't really rely on
	 * normal C++ behavior for your data to be freed in the order that you
	 * expect or want it to. */
	QApplication::sendPostedEvents(nullptr);

#ifdef BROWSER_AVAILABLE
	DestroyPanelCookieManager();
	delete cef;
	cef = nullptr;
#endif

	handledShutdown = true;
}

void OBSBasic::toggleMixerLayout()
{
	bool vertical = config_get_bool(App()->GetUserConfig(), "BasicWindow", "VerticalVolumeControl");
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "VerticalVolumeControl", !vertical);

	emit userSettingChanged("BasicWindow", "VerticalVolumeControl");
}

static inline int AttemptToResetVideo(struct obs_video_info *ovi)
{
	return obs_reset_video(ovi);
}

static inline enum obs_scale_type GetScaleType(ConfigFile &activeConfiguration)
{
	const char *scaleTypeStr = config_get_string(activeConfiguration, "Video", "ScaleType");

	if (astrcmpi(scaleTypeStr, "bilinear") == 0) {
		return OBS_SCALE_BILINEAR;
	} else if (astrcmpi(scaleTypeStr, "lanczos") == 0) {
		return OBS_SCALE_LANCZOS;
	} else if (astrcmpi(scaleTypeStr, "area") == 0) {
		return OBS_SCALE_AREA;
	} else {
		return OBS_SCALE_BICUBIC;
	}
}

static inline enum video_format GetVideoFormatFromName(const char *name)
{
	if (astrcmpi(name, "I420") == 0) {
		return VIDEO_FORMAT_I420;
	} else if (astrcmpi(name, "NV12") == 0) {
		return VIDEO_FORMAT_NV12;
	} else if (astrcmpi(name, "I444") == 0) {
		return VIDEO_FORMAT_I444;
	} else if (astrcmpi(name, "I010") == 0) {
		return VIDEO_FORMAT_I010;
	} else if (astrcmpi(name, "P010") == 0) {
		return VIDEO_FORMAT_P010;
	} else if (astrcmpi(name, "P216") == 0) {
		return VIDEO_FORMAT_P216;
	} else if (astrcmpi(name, "P416") == 0) {
		return VIDEO_FORMAT_P416;
	}
#if 0 //currently unsupported
	else if (astrcmpi(name, "YVYU") == 0)
		return VIDEO_FORMAT_YVYU;
	else if (astrcmpi(name, "YUY2") == 0)
		return VIDEO_FORMAT_YUY2;
	else if (astrcmpi(name, "UYVY") == 0)
		return VIDEO_FORMAT_UYVY;
#endif
	else {
		return VIDEO_FORMAT_BGRA;
	}
}

static inline enum video_colorspace GetVideoColorSpaceFromName(const char *name)
{
	enum video_colorspace colorspace = VIDEO_CS_SRGB;
	if (strcmp(name, "601") == 0) {
		colorspace = VIDEO_CS_601;
	} else if (strcmp(name, "709") == 0) {
		colorspace = VIDEO_CS_709;
	} else if (strcmp(name, "2100PQ") == 0) {
		colorspace = VIDEO_CS_2100_PQ;
	} else if (strcmp(name, "2100HLG") == 0) {
		colorspace = VIDEO_CS_2100_HLG;
	}

	return colorspace;
}

int OBSBasic::ResetVideo()
{
	if (outputHandler && outputHandler->Active()) {
		return OBS_VIDEO_CURRENTLY_ACTIVE;
	}

	ProfileScope("OBSBasic::ResetVideo");

	struct obs_video_info ovi;
	int ret;

	GetConfigFPS(ovi.fps_num, ovi.fps_den);

	const char *colorFormat = config_get_string(activeConfiguration, "Video", "ColorFormat");
	const char *colorSpace = config_get_string(activeConfiguration, "Video", "ColorSpace");
	const char *colorRange = config_get_string(activeConfiguration, "Video", "ColorRange");

	ovi.graphics_module = App()->GetRenderModule();
	ovi.base_width = (uint32_t)config_get_uint(activeConfiguration, "Video", "BaseCX");
	ovi.base_height = (uint32_t)config_get_uint(activeConfiguration, "Video", "BaseCY");
	ovi.output_width = (uint32_t)config_get_uint(activeConfiguration, "Video", "OutputCX");
	ovi.output_height = (uint32_t)config_get_uint(activeConfiguration, "Video", "OutputCY");
	ovi.output_format = GetVideoFormatFromName(colorFormat);
	ovi.colorspace = GetVideoColorSpaceFromName(colorSpace);
	ovi.range = astrcmpi(colorRange, "Full") == 0 ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	ovi.adapter = config_get_uint(App()->GetUserConfig(), "Video", "AdapterIdx");
	ovi.gpu_conversion = true;
	ovi.scale_type = GetScaleType(activeConfiguration);

	if (ovi.base_width < 32 || ovi.base_height < 32) {
		ovi.base_width = 1920;
		ovi.base_height = 1080;
		config_set_uint(activeConfiguration, "Video", "BaseCX", 1920);
		config_set_uint(activeConfiguration, "Video", "BaseCY", 1080);
	}

	if (ovi.output_width < 32 || ovi.output_height < 32) {
		ovi.output_width = ovi.base_width;
		ovi.output_height = ovi.base_height;
		config_set_uint(activeConfiguration, "Video", "OutputCX", ovi.base_width);
		config_set_uint(activeConfiguration, "Video", "OutputCY", ovi.base_height);
	}

	ret = AttemptToResetVideo(&ovi);
	if (ret == OBS_VIDEO_CURRENTLY_ACTIVE) {
		blog(LOG_WARNING, "Tried to reset when already active");
		return ret;
	}

	if (ret == OBS_VIDEO_SUCCESS) {
		ResizePreview(ovi.base_width, ovi.base_height);
		if (program) {
			ResizeProgram(ovi.base_width, ovi.base_height);
		}

		const float sdr_white_level = (float)config_get_uint(activeConfiguration, "Video", "SdrWhiteLevel");
		const float hdr_nominal_peak_level =
			(float)config_get_uint(activeConfiguration, "Video", "HdrNominalPeakLevel");
		obs_set_video_levels(sdr_white_level, hdr_nominal_peak_level);
		OBSBasicStats::InitializeValues();
		OBSProjector::UpdateMultiviewProjectors();

		if (!collections.empty()) {
			const OBS::SceneCollection currentSceneCollection = OBSBasic::GetCurrentSceneCollection();

			bool usingAbsoluteCoordinates = currentSceneCollection.getCoordinateMode() ==
							OBS::SceneCoordinateMode::Absolute;
			OBS::Rect migrationResolution = currentSceneCollection.getMigrationResolution();

			OBS::Rect videoResolution = OBS::Rect(ovi.base_width, ovi.base_height);

			bool canMigrate = usingAbsoluteCoordinates ||
					  (!migrationResolution.isZero() && migrationResolution != videoResolution);

			ui->actionRemigrateSceneCollection->setEnabled(canMigrate);
		} else {
			ui->actionRemigrateSceneCollection->setEnabled(false);
		}

		emit CanvasResized(ovi.base_width, ovi.base_height);
		emit OutputResized(ovi.output_width, ovi.output_height);
	}

	return ret;
}

bool OBSBasic::ResetAudio()
{
	ProfileScope("OBSBasic::ResetAudio");

	struct obs_audio_info2 ai = {};
	ai.samples_per_sec = config_get_uint(activeConfiguration, "Audio", "SampleRate");

	const char *channelSetupStr = config_get_string(activeConfiguration, "Audio", "ChannelSetup");

	if (strcmp(channelSetupStr, "Mono") == 0) {
		ai.speakers = SPEAKERS_MONO;
	} else if (strcmp(channelSetupStr, "2.1") == 0) {
		ai.speakers = SPEAKERS_2POINT1;
	} else if (strcmp(channelSetupStr, "4.0") == 0) {
		ai.speakers = SPEAKERS_4POINT0;
	} else if (strcmp(channelSetupStr, "4.1") == 0) {
		ai.speakers = SPEAKERS_4POINT1;
	} else if (strcmp(channelSetupStr, "5.1") == 0) {
		ai.speakers = SPEAKERS_5POINT1;
	} else if (strcmp(channelSetupStr, "7.1") == 0) {
		ai.speakers = SPEAKERS_7POINT1;
	} else {
		ai.speakers = SPEAKERS_STEREO;
	}

	bool lowLatencyAudioBuffering = config_get_bool(App()->GetUserConfig(), "Audio", "LowLatencyAudioBuffering");
	if (lowLatencyAudioBuffering) {
		ai.max_buffering_ms = 20;
		ai.fixed_buffering = true;
	}

	return obs_reset_audio2(&ai);
}

void OBSBasic::close()
{
	if (isClosePromptOpen() || isClosing()) {
		return;
	}

	OBSMainWindow::close();
}

void OBSBasic::closeEvent(QCloseEvent *event)
{
	if (closeAfterOutputsStop_) {
		event->ignore();
		return;
	}
	if (isClosePromptOpen() || isClosing()) {
		return;
	}

	if (isVisible()) {
		config_set_string(App()->GetUserConfig(), "BasicWindow", "geometry",
				  saveGeometry().toBase64().constData());
	}

	if (!isReadyToClose()) {
		event->ignore();

		QTimer::singleShot(1000, this, &OBSBasic::close);
		return;
	}

	if (remux && !remux->close()) {
		event->ignore();
		restart = false;

		return;
	}

	if (shouldPromptForClose()) {
		event->ignore();
		restart = false;

		if (!isClosePromptOpen()) {
			bool shouldClose = promptToClose();

			if (shouldClose) {
				closeWindow();
			}
		}

		return;
	}

	QWidget::closeEvent(event);
	if (!event->isAccepted()) {
		return;
	}

	closeWindow();
	if (!isClosing()) {
		event->ignore();
	}
}

bool OBSBasic::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_MOVE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnMove();
		}
		break;
	case WM_DISPLAYCHANGE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnDisplayChange();
		}
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

void OBSBasic::changeEvent(QEvent *event)
{
	if (event->type() == QEvent::WindowStateChange) {
		QWindowStateChangeEvent *stateEvent = (QWindowStateChangeEvent *)event;

		if (isMinimized()) {
			if (trayIcon && trayIcon->isVisible() && sysTrayMinimizeToTray()) {
				ToggleShowHide();
				return;
			}

			if (previewEnabled) {
				EnablePreviewDisplay(false);
			}
		} else if (stateEvent->oldState() & Qt::WindowMinimized && isVisible()) {
			if (previewEnabled) {
				EnablePreviewDisplay(true);
			}
		}
	}
}

void OBSBasic::GetFPSCommon(uint32_t &num, uint32_t &den) const
{
	const char *val = config_get_string(activeConfiguration, "Video", "FPSCommon");

	if (strcmp(val, "10") == 0) {
		num = 10;
		den = 1;
	} else if (strcmp(val, "20") == 0) {
		num = 20;
		den = 1;
	} else if (strcmp(val, "24 NTSC") == 0) {
		num = 24000;
		den = 1001;
	} else if (strcmp(val, "25 PAL") == 0) {
		num = 25;
		den = 1;
	} else if (strcmp(val, "29.97") == 0) {
		num = 30000;
		den = 1001;
	} else if (strcmp(val, "48") == 0) {
		num = 48;
		den = 1;
	} else if (strcmp(val, "50 PAL") == 0) {
		num = 50;
		den = 1;
	} else if (strcmp(val, "59.94") == 0) {
		num = 60000;
		den = 1001;
	} else if (strcmp(val, "60") == 0) {
		num = 60;
		den = 1;
	} else {
		num = 30;
		den = 1;
	}
}

void OBSBasic::GetFPSInteger(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSInt");
	den = 1;
}

void OBSBasic::GetFPSFraction(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSNum");
	den = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSDen");
}

void OBSBasic::GetFPSNanoseconds(uint32_t &num, uint32_t &den) const
{
	num = 1000000000;
	den = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSNS");
}

void OBSBasic::GetConfigFPS(uint32_t &num, uint32_t &den) const
{
	uint32_t type = config_get_uint(activeConfiguration, "Video", "FPSType");

	if (type == 1) { //"Integer"
		GetFPSInteger(num, den);
	} else if (type == 2) { //"Fraction"
		GetFPSFraction(num, den);
	}
	/*
	 * 	else if (false) //"Nanoseconds", currently not implemented
	 *		GetFPSNanoseconds(num, den);
	 */
	else {
		GetFPSCommon(num, den);
	}
}

config_t *OBSBasic::Config() const
{
	return activeConfiguration;
}

void OBSBasic::saveAll()
{
	if (!dockStatePersistenceReady_) {
		dockStatePersistenceReady_ = true;
		if (dockStateRestoreFailed_)
			lastSavedDockState_ = saveState();
	}
	SaveDockState();

	if (isVisible()) {
		config_set_string(App()->GetUserConfig(), "BasicWindow", "geometry",
				  saveGeometry().toBase64().constData());
	}

	std::call_once(saveOnceFlag, [this]() {
		Auth::Save();
		SaveProjectNow();
	});

	config_set_int(App()->GetAppConfig(), "General", "LastVersion", LIBOBS_API_VER);
	config_save_safe(App()->GetAppConfig(), "tmp", nullptr);

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "PreviewEnabled", previewEnabled);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "AlwaysOnTop", ui->actionAlwaysOnTop->isChecked());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SceneDuplicationMode", sceneDuplicationMode);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SwapScenesMode", swapScenesMode);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "EditPropertiesMode", editPropertiesMode);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "PreviewProgramMode", IsPreviewProgramMode());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "DocksLocked", ui->lockDocks->isChecked());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks", ui->sideDocks->isChecked());
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
}

bool OBSBasic::isReadyToClose()
{
	/* Wait for multitrack video stream to start/finish processing in the background */
	if (setupStreamingGuard.valid() &&
	    setupStreamingGuard.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
		return false;
	}

	/* Do not close window if inside of a temporary event loop because we
	 * could be inside of an Auth::LoadUI call.  Keep trying once per
	 * second until we've exit any known sub-loops. */
	if (os_atomic_load_long(&insideEventLoop) != 0) {
		return false;
	}

#ifdef YOUTUBE_ENABLED
	/* Also don't close the window if the youtube stream check is active */
	if (youtubeStreamCheckThread) {
		return false;
	}
#endif

	return true;
}

bool OBSBasic::shouldPromptForClose()
{
	bool confirmOnExit = config_get_bool(App()->GetUserConfig(), "General", "ConfirmOnExit");
	if (confirmOnExit && !clearingFailed &&
	    ((outputHandler && outputHandler->Active()) || MultiStreamManager::Instance().HasActiveOutputs())) {
		return true;
	}

	return false;
}

bool OBSBasic::promptToClose()
{
	isClosePromptOpen_ = true;

	SetShowing(true);
	QMessageBox::StandardButton button =
		OBSMessageBox::question(this, QTStr("ConfirmExit.Title"), QTStr("ConfirmExit.Text"),
					QMessageBox::StandardButtons(QMessageBox::Ok | QMessageBox::Cancel));

	if (button == QMessageBox::Cancel) {
		isClosePromptOpen_ = false;
		return false;
	}

	isClosePromptOpen_ = false;
	return true;
}

void OBSBasic::closeWindow()
{
	if (isClosing()) {
		return;
	}

	// Keep sources, encoders and the event loop alive until queued data has
	// reached each output's normal stop timestamp. Destroying them first
	// turns shutdown into a forced stop and can discard the stream's tail.
	if (!closeAfterOutputsStop_) {
		closeAfterOutputsStop_ = true;
		disableOutputsRef++;
		if ((outputHandler && outputHandler->Active()) || MultiStreamManager::Instance().HasActiveOutputs())
			blog(LOG_INFO, "[HK Stream] Finishing output data before closing");
		if (outputHandler) {
			if (outputHandler->StreamingActive() || outputHandler->delayActive) {
				StopStreaming();
			}
			if (outputHandler->RecordingActive()) {
				outputHandler->StopRecording(false);
			}
			outputHandler->StopSecondaryRecording(false);
			if (outputHandler->ReplayBufferActive()) {
				outputHandler->StopReplayBuffer(false);
			}
		}
	}
	MultiStreamManager::Instance().StopAllOutputs();

	const bool outputsPending = outputHandler &&
		(outputHandler->StreamingActive() || outputHandler->delayActive ||
		 outputHandler->RecordingActive() || outputHandler->ReplayBufferActive() ||
		 obs_output_active(outputHandler->secondaryFileOutput));
	if (outputsPending || MultiStreamManager::Instance().HasActiveOutputs()) {
		QTimer::singleShot(100, this, &OBSBasic::closeWindow);
		return;
	}

	blog(LOG_INFO, SHUTDOWN_SEPARATOR);

	isClosing_ = true;

	/* While closing, a resize event to OBSQTDisplay could be triggered.
	 * The graphics thread on macOS dispatches a lambda function to be
	 * executed asynchronously in the main thread. However, the display is
	 * sometimes deleted before the lambda function is actually executed.
	 * To avoid such a case, destroy displays earlier than others such as
	 * deleting browser docks. */
	ui->preview->DestroyDisplay();
	if (program) {
		program->DestroyDisplay();
	}

	if (outputHandler->VirtualCamActive()) {
		outputHandler->StopVirtualCam();
	}

	if (introCheckThread) {
		introCheckThread->wait();
	}
	if (whatsNewInitThread) {
		whatsNewInitThread->wait();
	}
	if (updateCheckThread) {
		updateCheckThread->wait();
	}
	if (logUploadThread) {
		logUploadThread->wait();
	}
	if (devicePropertiesThread && devicePropertiesThread->isRunning()) {
		devicePropertiesThread->wait();
		devicePropertiesThread.reset();
	}

	QApplication::sendPostedEvents(nullptr);

	signalHandlers.clear();
	delete extraBrowsers;

	// Temporary titles are runtime-only. Restore the saved title source settings
	// before SaveProjectNow serializes the current scene collection.
	TitleManagerDialog::RestoreTemporaryTitleFromOBS();

	saveAll();

#ifdef _WIN32
	/* HK Stream: everything the user cares about is saved at this point. Tearing
	 * the sources down can block for minutes when a capture driver (for example
	 * the audio side of a DirectShow capture card, stuck in waveInOpen) holds the
	 * Windows audio engine, because the WASAPI sources wait for it while they are
	 * destroyed. That is how the program stayed "not responding" after pressing
	 * close. Give the normal shutdown time, then end the process instead of
	 * waiting for the driver forever. */
	std::thread([]() {
		std::this_thread::sleep_for(std::chrono::seconds(20));
		blog(LOG_WARNING, "[HK Stream] Shutdown did not finish within 20 s (a capture driver is blocking "
				  "source teardown). Everything was already saved, ending the process.");
		TerminateProcess(GetCurrentProcess(), 0);
	}).detach();
#endif

	auth.reset();

#ifdef BROWSER_AVAILABLE
	ClearExtraBrowserDocks();

#ifdef YOUTUBE_ENABLED
	if (youtubeAppDock) {
		RemoveDockWidget(youtubeAppDock->objectName());
		youtubeAppDock = nullptr;
	}
#endif
#endif

	OnEvent(OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN);

	disableSaving++;

	/* Clear all scene data (dialogs, widgets, widget sub-items, scenes,
	 * sources, etc) so that all references are released before shutdown */
	ClearSceneData();

	OnEvent(OBS_FRONTEND_EVENT_EXIT);

	// Destroys the frontend API so plugins can't continue calling it
	obs_frontend_set_callbacks_internal(nullptr);
	api = nullptr;

	applicationShutdown();

#ifndef __APPLE__
	deleteLater();

	emit mainWindowClosed();

	QMetaObject::invokeMethod(App(), &OBSApp::quit, Qt::QueuedConnection);
#else
	QMetaObject::invokeMethod(App(), &OBSApp::quit, Qt::QueuedConnection);
#endif
}

void OBSBasic::UpdateEditMenu()
{
	QModelIndexList items = GetAllSelectedSourceItems();
	int totalCount = items.count();
	size_t filter_count = 0;

	if (totalCount == 1) {
		OBSSceneItem sceneItem = ui->sources->Get(GetTopSelectedSourceItem());
		OBSSource source = obs_sceneitem_get_source(sceneItem);
		filter_count = obs_source_filter_count(source);
	}

	int videoCount = 0;
	bool canTransformMultiple = false;
	for (int i = 0; i < totalCount; i++) {
		OBSSceneItem item = ui->sources->Get(items.value(i).row());
		OBSSource source = obs_sceneitem_get_source(item);
		const uint32_t flags = obs_source_get_output_flags(source);
		const bool hasVideo = (flags & OBS_SOURCE_VIDEO) != 0;
		if (hasVideo && !obs_sceneitem_locked(item)) {
			canTransformMultiple = true;
		}

		if (hasVideo) {
			videoCount++;
		}
	}
	const bool canTransformSingle = videoCount == 1 && totalCount == 1;

	OBSSceneItem curItem = GetCurrentSceneItem();
	bool locked = curItem && obs_sceneitem_locked(curItem);

	ui->actionCopySource->setEnabled(totalCount > 0);
	ui->actionEditTransform->setEnabled(canTransformSingle && !locked);
	ui->actionCopyTransform->setEnabled(canTransformSingle);
	ui->actionPasteTransform->setEnabled(canTransformMultiple && hasCopiedTransform && videoCount > 0);
	ui->actionCopyFilters->setEnabled(filter_count > 0);
	ui->actionPasteFilters->setEnabled(!obs_weak_source_expired(copyFiltersSource()) && totalCount > 0);
	auto pasteType = getItemPasteType();
	ui->actionPasteRef->setEnabled(pasteType == OBS::ItemPasteType::Reference ||
				       pasteType == OBS::ItemPasteType::Both);
	ui->actionPasteDup->setEnabled(pasteType == OBS::ItemPasteType::Duplicate ||
				       pasteType == OBS::ItemPasteType::Both);

	ui->actionMoveUp->setEnabled(totalCount > 0);
	ui->actionMoveDown->setEnabled(totalCount > 0);
	ui->actionMoveToTop->setEnabled(totalCount > 0);
	ui->actionMoveToBottom->setEnabled(totalCount > 0);

	ui->actionResetTransform->setEnabled(canTransformMultiple);
	ui->actionRotate90CW->setEnabled(canTransformMultiple);
	ui->actionRotate90CCW->setEnabled(canTransformMultiple);
	ui->actionRotate180->setEnabled(canTransformMultiple);
	ui->actionFlipHorizontal->setEnabled(canTransformMultiple);
	ui->actionFlipVertical->setEnabled(canTransformMultiple);
	ui->actionFitToScreen->setEnabled(canTransformMultiple);
	ui->actionStretchToScreen->setEnabled(canTransformMultiple);
	ui->actionCenterToScreen->setEnabled(canTransformMultiple);
	ui->actionVerticalCenter->setEnabled(canTransformMultiple);
	ui->actionHorizontalCenter->setEnabled(canTransformMultiple);
}

void OBSBasic::UpdateTitleBar()
{
	stringstream name;

	const char *profile = config_get_string(App()->GetUserConfig(), "Basic", "Profile");
	const char *sceneCollection = config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection");

	name << "HK Stream";
	if (workspaceManager_ && workspaceManager_->GetActive()) {
		name << " - [" << workspaceManager_->GetActive()->name << "]";
	}

	obs_source_t *curScene = obs_frontend_get_current_scene();
	if (curScene) {
		name << " - " << obs_source_get_name(curScene);
		obs_source_release(curScene);
	}

	if (previewProgramMode) {
		name << " (Studio Mode)";
	}

	if (safe_mode) {
		name << " (" << Str("TitleBar.SafeMode") << ")";
	}
	if (App()->IsPortableMode()) {
		name << " - " << Str("TitleBar.PortableMode");
	}

	setWindowTitle(QT_UTF8(name.str().c_str()));
}

OBSBasic *OBSBasic::Get()
{
	return reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
}

void OBSBasic::UpdatePatronJson(const std::string &text, const std::string &error)
{
	if (!error.empty()) {
		return;
	}

	patronJson = text;
}

void OBSBasic::SetDisplayAffinity(QWindow *window)
{
	if (!SetDisplayAffinitySupported()) {
		return;
	}

	bool hideFromCapture = config_get_bool(App()->GetUserConfig(), "BasicWindow", "HideOBSWindowsFromCapture");

	// Don't hide projectors, those are designed to be visible / captured
	if (window->property("isOBSProjectorWindow") == true) {
		return;
	}

#ifdef _WIN32
	HWND hwnd = (HWND)window->winId();

	DWORD curAffinity;
	if (GetWindowDisplayAffinity(hwnd, &curAffinity)) {
		if (hideFromCapture && curAffinity != WDA_EXCLUDEFROMCAPTURE) {
			SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
		} else if (!hideFromCapture && curAffinity != WDA_NONE) {
			SetWindowDisplayAffinity(hwnd, WDA_NONE);
		}
	}

#else
	// TODO: Implement for other platforms if possible. Don't forget to
	// implement SetDisplayAffinitySupported too!
	UNUSED_PARAMETER(hideFromCapture);
#endif
}

void OBSBasic::OnEvent(enum obs_frontend_event event)
{
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		obs_source_t *cur = GetCurrentSceneSource();
		if (outputHandler && outputHandler->SecondaryRecordingActive()) {
			outputHandler->UpdateSecondaryRecordingScene(cur);
		}
		MultiStreamManager::Instance().UpdateSecondaryStreamScenes(cur);
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		UpdateTitleQuickCombo();
		const QString activeTitle = TitleManagerDialog::GetActiveTitle().trimmed();
		const auto entries = TitleManagerDialog::LoadTitlesData();
		for (const auto &entry : entries) {
			if (entry.text.trimmed() == activeTitle) {
				TitleManagerDialog::ApplyTitleToOBS(entry.text, entry.fontFace, entry.fontSize, entry.isBold);
				break;
			}
		}
	}

	if (api) {
		api->on_event(event);
	}
}

OBSPromptResult OBSBasic::PromptForName(const OBSPromptRequest &request, const OBSPromptCallback &callback)
{
	OBSPromptResult result;

	for (;;) {
		result.success = false;

		if (request.withOption && !request.optionPrompt.empty()) {
			result.optionValue = request.optionValue;

			result.success = NameDialog::AskForNameWithOption(
				this, request.title.c_str(), request.prompt.c_str(), result.promptValue,
				request.optionPrompt.c_str(), result.optionValue,
				(request.promptValue.empty() ? nullptr : request.promptValue.c_str()));

		} else {
			result.success = NameDialog::AskForName(
				this, request.title.c_str(), request.prompt.c_str(), result.promptValue,
				(request.promptValue.empty() ? nullptr : request.promptValue.c_str()));
		}

		if (!result.success) {
			break;
		}

		if (result.promptValue.empty()) {
			OBSMessageBox::warning(this, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
			continue;
		}

		if (!callback(result)) {
			OBSMessageBox::warning(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
			continue;
		}

		break;
	}

	return result;
}

void OBSBasic::on_actionOpenPluginManager_triggered()
{
	App()->pluginManagerOpenDialog();
}

bool OBSBasic::ApplyActiveWorkspaceFps(uint32_t num, uint32_t den)
{
	if (!workspaceManager_ || num == 0 || den == 0)
		return false;
	if ((outputHandler && outputHandler->Active()) || MultiStreamManager::Instance().HasActiveOutputs())
		return false;
	const OBSWorkspace *active = workspaceManager_->GetActive();
	OBSWorkspace *ws = active ? workspaceManager_->GetById(active->id) : nullptr;
	if (!ws)
		return false;
	ws->video.fps_num = num;
	ws->video.fps_den = den;
	workspaceManager_->Save();
	ApplyWorkspaceVideo(*ws);
	return true;
}

void OBSBasic::ApplyWorkspaceVideo(const OBSWorkspace &ws)
{
	InstantSceneChangeScope instantSceneChange(this);

	if ((outputHandler && outputHandler->Active()) || MultiStreamManager::Instance().HasActiveOutputs()) {
		blog(LOG_WARNING, "[Workspaces] Cannot reset video while streaming/recording");
		return;
	}

	config_set_uint(activeConfiguration, "Video", "BaseCX", ws.video.base_cx);
	config_set_uint(activeConfiguration, "Video", "BaseCY", ws.video.base_cy);
	config_set_uint(activeConfiguration, "Video", "OutputCX", ws.video.output_cx);
	config_set_uint(activeConfiguration, "Video", "OutputCY", ws.video.output_cy);
	config_set_uint(activeConfiguration, "Video", "FPSNum", ws.video.fps_num);
	config_set_uint(activeConfiguration, "Video", "FPSDen", ws.video.fps_den);

	// Resetting video stops and restarts rendering, so skip it when the canvas
	// already has this workspace's size and frame rate.
	obs_video_info current{};
	uint32_t fpsNum = 0;
	uint32_t fpsDen = 0;
	GetConfigFPS(fpsNum, fpsDen);
	const bool unchanged = obs_get_video_info(&current) && current.base_width == ws.video.base_cx &&
			       current.base_height == ws.video.base_cy && current.output_width == ws.video.output_cx &&
			       current.output_height == ws.video.output_cy && current.fps_num == fpsNum &&
			       current.fps_den == fpsDen;

	int ret = unchanged ? OBS_VIDEO_SUCCESS : ResetVideo();
	blog(LOG_INFO, "[Workspaces] ResetVideo for workspace '%s': %ux%u -> %s %d", ws.name.c_str(),
	     ws.video.base_cx, ws.video.base_cy, unchanged ? "unchanged" : "ret", ret);

	// Filter ui->scenes to only show scenes belonging to the active workspace
	if (ui->scenes) {
		for (int i = 0; i < ui->scenes->count(); ++i) {
			QListWidgetItem *it = ui->scenes->item(i);
			if (!it) continue;
			std::string sName = QT_TO_UTF8(it->text());
			bool inWorkspace = std::find(ws.scenes.begin(), ws.scenes.end(), sName) != ws.scenes.end();
			ui->scenes->setRowHidden(i, !inWorkspace);
		}
	}

	// Strictly enforce: Current scene MUST belong to the active workspace!
	OBSSource curScene = GetCurrentSceneSource();
	std::string curName = curScene ? obs_source_get_name(curScene) : "";
	const std::string logicalCurName = OBSWorkspaceManager::GetLogicalSceneName(curName);
	const bool curInWs = !logicalCurName.empty() &&
		std::find(ws.scenes.begin(), ws.scenes.end(), logicalCurName) != ws.scenes.end();

	// Resolution resets used to treat the tagged per-size backing scene as a
	// foreign scene and fall back to the untagged logical scene. That put edits
	// back onto the shared template, so moving an item in one format could leak
	// into every other format. Always keep the active logical scene bound to its
	// own workspace + exact-resolution backing scene after ResetVideo().
	std::string logicalTarget = curInWs ? logicalCurName : ws.active_scene;
	if (logicalTarget.empty() && !ws.scenes.empty())
		logicalTarget = ws.scenes.front();
	if (!curInWs && logicalTarget.empty() && ui->scenes) {
		for (int i = 0; i < ui->scenes->count(); ++i) {
			if (!ui->scenes->isRowHidden(i)) {
				QListWidgetItem *it = ui->scenes->item(i);
				if (it) {
					logicalTarget = QT_TO_UTF8(it->text());
					break;
				}
			}
		}
	}

	std::string workspaceBackingName;
	if (!logicalTarget.empty()) {
		const std::string aspect = OBSWorkspaceManager::GetAspectRatioKey(ws.video.base_cx, ws.video.base_cy);
		std::string backingName;
		{
			ProfileScope("OBSWorkspaceManager::GetOrCreateBackingScene");
			backingName = OBSWorkspaceManager::GetOrCreateBackingScene(logicalTarget, aspect, ws.id,
										    ws.video.base_cx, ws.video.base_cy);
		}
		obs_source_t *targetScene = obs_get_source_by_name(backingName.c_str());
		if (targetScene) {
			ProfileScope("OBSBasic::SetCurrentScene (workspace)");
			if (curName != backingName)
				SetCurrentScene(targetScene, true);
			obs_source_release(targetScene);
		}
		workspaceBackingName = backingName;
	}

	if (ui->scenes) {
		for (int i = 0; i < ui->scenes->count(); ++i) {
			QListWidgetItem *it = ui->scenes->item(i);
			if (it && QT_TO_UTF8(it->text()) == logicalTarget) {
				ui->scenes->setCurrentItem(it);
				break;
			}
		}
	}

	// Selecting the logical scene in the list above makes it the current scene
	// until the queued correction in WorkspacePanel::OnOBSSceneChanged runs, and
	// the logical scene lays its items out differently, so it was visible for a
	// moment. Put the backing scene back right away so it is never shown.
	if (!workspaceBackingName.empty()) {
		OBSSourceAutoRelease backingScene = obs_get_source_by_name(workspaceBackingName.c_str());
		OBSSource shownScene = GetCurrentSceneSource();
		if (backingScene && shownScene.Get() != backingScene.Get())
			SetCurrentScene(backingScene.Get(), true);
	}

	if (sourceCategoryFilter_) {
		FilterSourcesByCategory(sourceCategoryFilter_->currentIndex());
	}

	// Repair the known wide lesson backings whenever a 1920x1080 canvas is
	// active, even if the current workspace is different. Older builds marked
	// these backings restored while omitting the public group source, so each
	// call merges only missing item IDs and preserves the surviving design.
	//
	// HK Stream: this repair is idempotent, so run it once per scene collection
	// per session. It used to run, and then save the whole 2.7 MB project, on
	// every single workspace/scene switch, which made rapid switching stall.
	bool lessonBackingsChecked = false;
	static std::string lessonBackingsRepairedFor;
	const char *activeCollectionName = config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection");
	const std::string collectionKey = activeCollectionName ? activeCollectionName : "";
	if (ws.video.base_cx == 1920 && ws.video.base_cy == 1080 && workspaceManager_ &&
	    workspaceManager_->GetById("ws_lesson") && lessonBackingsRepairedFor != collectionKey) {
		/* Only count the repair as a change (and save the whole project) when it
		 * really created a scene or added items; on a healthy collection it
		 * changes nothing and the save at every launch was wasted work. */
		auto sceneFingerprint = []() {
			struct State {
				size_t scenes = 0;
				size_t items = 0;
			} state;
			obs_enum_scenes(
				[](void *param, obs_source_t *source) {
					State *s = static_cast<State *>(param);
					s->scenes++;
					obs_scene_t *scene = obs_scene_from_source(source);
					if (scene) {
						obs_scene_enum_items(
							scene,
							[](obs_scene_t *, obs_sceneitem_t *, void *p) {
								(*static_cast<size_t *>(p))++;
								return true;
							},
							&s->items);
					}
					return true;
				},
				&state);
			return state.scenes * 1000003u + state.items;
		};
		const size_t before = sceneFingerprint();
		for (const char *lessonScene : {"درس", "درس شاشة فقط"})
			OBSWorkspaceManager::GetOrCreateBackingScene(lessonScene, "16:9", "ws_lesson", 1920, 1080);
		lessonBackingsRepairedFor = collectionKey;
		lessonBackingsChecked = sceneFingerprint() != before;
	}

	const bool cameraCropMigrated = OBSWorkspaceManager::MigrateSharedCameraCropToSceneItems();
	if (lessonBackingsChecked || cameraCropMigrated)
		SaveProject();
}

void OBSBasic::RestoreStreamSessionWorkspaceVideo()
{
	if (!streamSessionVideoOverrideActive_)
		return;
	if (outputHandler && outputHandler->Active())
		return; // A linked recording can outlive the stream; restore after its stop callback.
	if (MultiStreamManager::Instance().HasActiveOutputs()) {
		QTimer::singleShot(100, this, &OBSBasic::RestoreStreamSessionWorkspaceVideo);
		return;
	}

	streamSessionVideoOverrideActive_ = false;
	if (workspaceManager_ && !streamSessionWorkspaceId_.empty()) {
		OBSWorkspace *workspace = workspaceManager_->GetById(streamSessionWorkspaceId_);
		const OBSWorkspace *active = workspaceManager_->GetActive();
		if (workspace && active && active->id == streamSessionWorkspaceId_) {
			ApplyWorkspaceVideo(*workspace);
			if (workspacePanel_)
				workspacePanel_->RefreshAll();
		}
	}
	streamSessionWorkspaceId_.clear();
}

void OBSBasic::FilterSourcesByCategory(int tabIndex)
{
	if (!workspaceManager_ || !ui->sources) return;
	if (tabIndex < 0 && sourceCategoryFilter_)
		tabIndex = sourceCategoryFilter_->currentIndex();

	const OBSWorkspace *active = workspaceManager_->GetActive();
	if (!active) return;

	// 0: all, 1: core, 2: variable, 3: audio-only.
	if (tabIndex != 3)
		sourceEditingCategory_ = tabIndex;
	const int editingCategory = sourceEditingCategory_;

	if (sourceLockNotice_) {
		if (editingCategory == 2) {
			sourceLockNotice_->setText("🔒 وضع المتغيرة: الأساسية مقفولة ومحمية من الضغط والتحريك");
			sourceLockNotice_->setProperty("noticeMode", "variable");
			sourceLockNotice_->setVisible(true);
		} else if (editingCategory == 1) {
			sourceLockNotice_->setText("🛡️ وضع الأساسية: المتغيرة مقفولة ومحمية من الضغط والتحريك");
			sourceLockNotice_->setProperty("noticeMode", "core");
			sourceLockNotice_->setVisible(true);
		} else {
			sourceLockNotice_->setVisible(false);
		}
		sourceLockNotice_->style()->unpolish(sourceLockNotice_);
		sourceLockNotice_->style()->polish(sourceLockNotice_);
	}

	if (!ui->sources->model()) return;
	int rowCount = ui->sources->model()->rowCount();
	for (int i = 0; i < rowCount; ++i) {
		OBSSceneItem item = ui->sources->Get(i);
		if (!item) continue;
		obs_source_t *src = obs_sceneitem_get_source(item);
		if (!src) continue;
		const char *srcName = obs_source_get_name(src);
		if (!srcName) continue;
		const int category = workspaceManager_->GetSourceCategory(active->id, srcName);
		const bool isAudio = (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0;
		const bool show = tabIndex == 0 || (tabIndex == 1 && category == 0) ||
				  (tabIndex == 2 && category == 1) || (tabIndex == 3 && isAudio);

		// Reapply category locks on every switch. This also clears stale locks left
		// by the previous filter state after restarting the application.
		const bool belongsToEditableCategory = editingCategory == 0 ||
			(editingCategory == 1 && category == 0) || (editingCategory == 2 && category == 1);
		const bool lockedByCategory = !belongsToEditableCategory;
		obs_sceneitem_set_locked(item, lockedByCategory);
		if (lockedByCategory && obs_sceneitem_selected(item))
			obs_sceneitem_select(item, false);
		ui->sources->setRowHidden(i, !show);
	}
	RefreshInteractiveSourceSelectors();
}

void OBSBasic::RefreshSourceCategoryState()
{
	if (workspacePanel_)
		workspacePanel_->UpdateSmartDirectorConfig();
	if (sourceCategoryFilter_)
		FilterSourcesByCategory(sourceCategoryFilter_->currentIndex());
}

bool OBSBasic::IsVariableSourceInActiveWorkspace(obs_source_t *source) const
{
	if (!source || !workspaceManager_)
		return false;
	const OBSWorkspace *active = workspaceManager_->GetActive();
	const char *sourceName = obs_source_get_name(source);
	return active && sourceName && workspaceManager_->GetSourceCategory(active->id, sourceName) == 1;
}

bool OBSBasic::IsCurrentSceneInteractive()
{
	if (!workspaceManager_)
		return false;
	const OBSWorkspace *active = workspaceManager_->GetActive();
	OBSSource currentScene = GetCurrentSceneSource();
	const char *sceneName = currentScene ? obs_source_get_name(currentScene) : nullptr;
	return active && sceneName && workspaceManager_->IsInteractiveScene(
		active->id, OBSWorkspaceManager::GetLogicalSceneName(sceneName));
}

bool OBSBasic::IsInteractiveSourceForCurrentScene(obs_source_t *source)
{
	if (!source || !workspaceManager_)
		return false;
	const OBSWorkspace *active = workspaceManager_->GetActive();
	OBSSource currentScene = GetCurrentSceneSource();
	const char *sceneName = currentScene ? obs_source_get_name(currentScene) : nullptr;
	const char *sourceName = obs_source_get_name(source);
	if (!active || !sceneName || !sourceName)
		return false;
	const std::string logicalScene = OBSWorkspaceManager::GetLogicalSceneName(sceneName);
	const std::string logicalSource = OBSWorkspaceManager::GetLogicalSourceName(sourceName);
	return workspaceManager_->GetInteractiveSource(active->id, logicalScene) == logicalSource;
}

bool OBSBasic::CanSelectInteractiveSourceForCurrentScene()
{
	if (!workspaceManager_)
		return false;
	const OBSWorkspace *active = workspaceManager_->GetActive();
	OBSSource currentScene = GetCurrentSceneSource();
	const char *sceneName = currentScene ? obs_source_get_name(currentScene) : nullptr;
	if (!active || !sceneName)
		return false;

	const std::string logicalScene = OBSWorkspaceManager::GetLogicalSceneName(sceneName);
	if (workspaceManager_->IsBaseScene(active->id, logicalScene) ||
	    std::find(active->scenes.begin(), active->scenes.end(), logicalScene) == active->scenes.end())
		return false;

	auto isValidFallback = [&](const std::string &candidate) {
		return !candidate.empty() && candidate != logicalScene &&
			std::find(active->scenes.begin(), active->scenes.end(), candidate) != active->scenes.end() &&
			!workspaceManager_->IsInteractiveScene(active->id, candidate);
	};
	if (workspaceManager_->IsInteractiveScene(active->id, logicalScene))
		return isValidFallback(workspaceManager_->GetInteractiveFallbackScene(active->id, logicalScene));
	if (isValidFallback(workspaceManager_->GetBaseScene(active->id)))
		return true;
	return std::any_of(active->scenes.begin(), active->scenes.end(), isValidFallback);
}

bool OBSBasic::SetInteractiveSourceForCurrentScene(obs_source_t *source)
{
	if (!source || !IsVariableSourceInActiveWorkspace(source) || !CanSelectInteractiveSourceForCurrentScene())
		return false;
	const OBSWorkspace *active = workspaceManager_->GetActive();
	OBSSource currentScene = GetCurrentSceneSource();
	const char *sceneName = currentScene ? obs_source_get_name(currentScene) : nullptr;
	const char *sourceName = obs_source_get_name(source);
	if (!active || !sceneName || !sourceName)
		return false;

	const std::string logicalScene = OBSWorkspaceManager::GetLogicalSceneName(sceneName);
	const std::string logicalSource = OBSWorkspaceManager::GetLogicalSourceName(sourceName);
	const std::string conflictingScene = workspaceManager_->FindConflictSceneForSource(
		active->id, logicalSource, logicalScene);
	if (!conflictingScene.empty()) {
		QMessageBox::warning(this, QString::fromUtf8("المصدر التفاعلي"),
			QString::fromUtf8("هذا المصدر مرتبط مسبقًا بالمشهد «%1».")
				.arg(QString::fromStdString(conflictingScene)));
		return false;
	}
	std::string fallbackScene;
	if (workspaceManager_->IsInteractiveScene(active->id, logicalScene))
		fallbackScene = workspaceManager_->GetInteractiveFallbackScene(active->id, logicalScene);
	else {
		const std::string baseScene = workspaceManager_->GetBaseScene(active->id);
		if (baseScene != logicalScene &&
		    std::find(active->scenes.begin(), active->scenes.end(), baseScene) != active->scenes.end() &&
		    !workspaceManager_->IsInteractiveScene(active->id, baseScene))
			fallbackScene = baseScene;
		else {
			for (const std::string &candidate : active->scenes) {
				if (candidate != logicalScene && !workspaceManager_->IsInteractiveScene(active->id, candidate)) {
					fallbackScene = candidate;
					break;
				}
			}
		}
	}
	if (fallbackScene.empty())
		return false;

	const std::string workspaceId = active->id;
	workspaceManager_->SetInteractiveScene(workspaceId, logicalScene, logicalSource, fallbackScene);
	if (workspaceManager_->GetInteractiveSource(workspaceId, logicalScene) != logicalSource)
		return false;
	if (workspacePanel_)
		workspacePanel_->UpdateSmartDirectorConfig();
	if (workspacePanel_)
		workspacePanel_->RefreshAll();
	RefreshInteractiveSourceSelectors();
	return true;
}

void OBSBasic::SyncPairedSceneItemVisibility(obs_sceneitem_t *item, bool visible)
{
	if (syncingPairedSceneVisibility_ || !item || !workspaceManager_)
		return;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || !IsVariableSourceInActiveWorkspace(source))
		return;
	if (!ui || !ui->scenes)
		return;
	OBSSource currentSceneSource;
	for (int i = 0; i < ui->scenes->count(); ++i) {
		OBSScene rootScene = GetOBSRef<OBSScene>(ui->scenes->item(i));
		SceneItemIdentitySearch identity{item};
		obs_scene_enum_items(rootScene, FindSceneItemIdentity, &identity);
		if (identity.found) {
			currentSceneSource = obs_scene_get_source(rootScene);
			break;
		}
	}
	if (!currentSceneSource)
		return;
	WorkspaceSceneVariant currentVariant;
	if (!ParseWorkspaceSceneVariant(obs_source_get_name(currentSceneSource), currentVariant))
		return;
	const OBSWorkspace *workspace = workspaceManager_->GetActive();
	if (!workspace || workspace->id != currentVariant.workspaceId)
		return;
	const bool currentPortrait = currentVariant.height > currentVariant.width;
	const bool currentLandscape = currentVariant.width > currentVariant.height;
	if (!currentPortrait && !currentLandscape)
		return;

	struct SceneSearch {
		const WorkspaceSceneVariant *current;
		bool wantPortrait;
		std::vector<OBSScene> matches;
	} sceneSearch{&currentVariant, currentLandscape};
	obs_enum_sources(
		[](void *param, obs_source_t *candidate) {
			auto &search = *static_cast<SceneSearch *>(param);
			if (obs_source_removed(candidate))
				return true;
			obs_scene_t *scene = obs_scene_from_source(candidate);
			if (!scene)
				return true;
			WorkspaceSceneVariant variant;
			if (!ParseWorkspaceSceneVariant(obs_source_get_name(candidate), variant) ||
			    variant.logicalName != search.current->logicalName ||
			    variant.workspaceId != search.current->workspaceId)
				return true;
			const bool portrait = variant.height > variant.width;
			const bool landscape = variant.width > variant.height;
			if ((search.wantPortrait && portrait) || (!search.wantPortrait && landscape))
				search.matches.emplace_back(scene);
			return true;
		},
		&sceneSearch);
	if (sceneSearch.matches.size() != 1)
		return;

	MatchingSceneItems matches;
	const char *sourceName = obs_source_get_name(source);
	matches.logicalSource = OBSWorkspaceManager::GetLogicalSourceName(sourceName ? sourceName : "");
	obs_scene_enum_items(sceneSearch.matches.front(), FindMatchingSceneItems, &matches);
	if (matches.items.size() != 1 || obs_sceneitem_visible(matches.items.front()) == visible)
		return;

	syncingPairedSceneVisibility_ = true;
	obs_sceneitem_set_visible(matches.items.front(), visible);
	syncingPairedSceneVisibility_ = false;
	SaveProject();
}

void OBSBasic::RefreshInteractiveSourceSelectors()
{
	if (!ui || !ui->sources || !ui->sources->model())
		return;
	const int rowCount = ui->sources->model()->rowCount();
	for (int i = 0; i < rowCount; ++i) {
		SourceTreeItem *item = ui->sources->GetItemWidget(i);
		if (item)
			item->RefreshInteractiveSelector();
	}
}

bool OBSBasic::IsSceneItemEditableForCurrentCategory(obs_sceneitem_t *item) const
{
	if (!item || !workspaceManager_ || !sourceCategoryFilter_)
		return true;

	if (sourceEditingCategory_ == 0)
		return true;

	const OBSWorkspace *active = workspaceManager_->GetActive();
	if (!active)
		return false;

	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (!name)
		return false;

	const int category = workspaceManager_->GetSourceCategory(active->id, name);
	return sourceEditingCategory_ == 1 ? category == 0 : sourceEditingCategory_ == 2 && category == 1;
}

void OBSBasic::OpenTitleManager()
{
	TitleManagerDialog dlg(this, this);
	connect(&dlg, &TitleManagerDialog::titleChanged, this, [this](const QString &) {
		UpdateTitleQuickCombo();
	});
	dlg.exec();
	UpdateTitleQuickCombo();
}

void OBSBasic::UpdateTitleQuickCombo()
{
	if (!titleQuickCombo_) return;
	titleQuickCombo_->blockSignals(true);
	titleQuickCombo_->clear();
	auto data = TitleManagerDialog::LoadTitlesData();
	QString active = TitleManagerDialog::GetActiveTitle().trimmed();
	const QString temporary = TitleManagerDialog::GetTemporaryTitle().trimmed();

	int activeIdx = -1;
	if (!temporary.isEmpty()) {
		titleQuickCombo_->addItem(QString("⏱️ %1 (مؤقت)").arg(temporary), temporary);
		activeIdx = 0;
	}

	for (int i = 0; i < data.size(); ++i) {
		const QString displayTitle = data[i].isImage
			? QString::fromUtf8("🖼️ ") + data[i].text
			: data[i].text;
		titleQuickCombo_->addItem(displayTitle, data[i].text);
		if (temporary.isEmpty() && data[i].text.trimmed() == active) {
			activeIdx = titleQuickCombo_->count() - 1;
		}
	}

	if (activeIdx >= 0) {
		titleQuickCombo_->setCurrentIndex(activeIdx);
	}

	if (btnCancelTemp_) btnCancelTemp_->setVisible(!temporary.isEmpty());
	if (txtTempTitle_) {
		if (!temporary.isEmpty())
			txtTempTitle_->setText(temporary);
		else
			txtTempTitle_->clear();
	}
	titleQuickCombo_->blockSignals(false);

	if (obsHeader_) {
		if (!temporary.isEmpty()) {
			obsHeader_->setLessonNumberVisible(false);
		} else {
			TitleEntry entry = TitleManagerDialog::GetActiveTitleEntry();
			obsHeader_->setLessonNumberVisible(entry.hasNumber);
			if (entry.hasNumber) {
				obsHeader_->setLessonNumber(entry.lessonNumber);
			}
		}
	}
}

void OBSBasic::SetupWorkspaceMenu()
{
	if (workspaceMenu_)
		return;

	workspaceMenu_ = new QMenu(QString::fromUtf8("بيئات العمل"), ui->menubar);
	if (ui->menuTools) {
		ui->menubar->insertMenu(ui->menuTools->menuAction(), workspaceMenu_);
	} else {
		ui->menubar->addMenu(workspaceMenu_);
	}

	UpdateWorkspaceMenu();
}

void OBSBasic::UpdateWorkspaceMenu()
{
	if (!workspaceMenu_ || !workspaceManager_)
		return;

	workspaceMenu_->clear();

	// Action: Show Workspaces Panel Dock
	QAction *showDockAction = workspaceMenu_->addAction(
		QString::fromUtf8("📁 إظهار لوحة بيئات العمل"));
	connect(showDockAction, &QAction::triggered, this, [this]() {
		if (workspaceDock) {
			workspaceDock->setVisible(true);
			workspaceDock->raise();
		}
	});

	workspaceMenu_->addSeparator();

	// Workspaces list
	const OBSWorkspace *activeWs = workspaceManager_->GetActive();
	std::string activeId = activeWs ? activeWs->id : "";

	QActionGroup *group = new QActionGroup(workspaceMenu_);
	group->setExclusive(true);

	for (const auto &ws : workspaceManager_->GetWorkspaces()) {
		QString title = QString::fromStdString(ws.icon.empty() ? "🖥️ " : ws.icon + " ") +
		                QString::fromStdString(ws.name);
		title += QString("  (%1×%2)").arg(ws.video.base_cx).arg(ws.video.base_cy);

		QAction *action = workspaceMenu_->addAction(title);
		action->setCheckable(true);
		if (ws.id == activeId) {
			action->setChecked(true);
		}
		group->addAction(action);

		std::string wsId = ws.id;
		connect(action, &QAction::triggered, this, [this, wsId]() {
			if (workspacePanel_) {
				workspacePanel_->OnWorkspaceSelected(QString::fromStdString(wsId));
			} else if (workspaceManager_) {
				workspaceManager_->SwitchTo(wsId);
				if (const OBSWorkspace *active = workspaceManager_->GetActive()) {
					ApplyWorkspaceVideo(*active);
				}
				UpdateWorkspaceMenu();
				UpdateTitleBar();
			}
		});
	}

	workspaceMenu_->addSeparator();

	// Action: Open Workspaces Folder
	QAction *openFolderAction = workspaceMenu_->addAction(
		QString::fromUtf8("📂 فتح مجلد بيئات العمل"));
	connect(openFolderAction, &QAction::triggered, this, [this]() {
		QString wsDir = QString::fromStdString(OBSWorkspaceManager::GetWorkspacesDirPath());
		QDesktopServices::openUrl(QUrl::fromLocalFile(wsDir));
	});

	// Action: Open Source Categories Editor Dialog
	QAction *editSourcesAction = workspaceMenu_->addAction(
		QString::fromUtf8("⚙️ محرر مصادر بيئة العمل..."));
	connect(editSourcesAction, &QAction::triggered, this, &OBSBasic::OpenSourceCategoryDialog);
}

void OBSBasic::OpenSourceCategoryDialog()
{
	if (!workspaceManager_) return;
	SourceCategoryDialog dlg(workspaceManager_.get(), this, this);
	if (dlg.exec() == QDialog::Accepted) {
		if (workspacePanel_) workspacePanel_->UpdateSmartDirectorConfig();
		if (sourceCategoryFilter_) {
			FilterSourcesByCategory(sourceCategoryFilter_->currentIndex());
		}
	}
}

void OBSBasic::OpenWebSourceCodeDialog(obs_source_t *source)
{
	if (!source) return;
	WebSourceCodeDialog dlg(source, this, this);
	dlg.exec();
}

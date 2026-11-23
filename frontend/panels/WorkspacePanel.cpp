#include "WorkspacePanel.hpp"
#include "workspaces/WorkspaceModel.hpp"
#include "widgets/OBSBasic.hpp"
#include "dialogs/RecordingResolutionDialog.hpp"
#include "utility/SmartDirector.hpp"
#include "utility/RecycleBin.hpp"
#include "dialogs/RecycleBinDialog.hpp"
#include <QInputDialog>
#include <QMessageBox>
#include <obs.h>
#include <obs-frontend-api.h>
#include <QScrollArea>
#include <QStyle>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QApplication>
#include <QFontDatabase>
#include <QSizePolicy>
#include <QScopedValueRollback>
#include <QToolButton>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QColor>
#include <cmath>
#include <QFile>
#include <limits>
#include <QSvgRenderer>
#include <util/base.h>
#include <algorithm>
#include <Idian/ToggleSwitch.hpp>

namespace {
class SceneActionButton final : public QToolButton {
public:
	SceneActionButton(const QString &iconPath, const QString &accessibleName, QWidget *parent)
		: QToolButton(parent), iconPath_(iconPath)
	{
		QFile file(iconPath_);
		if (file.open(QIODevice::ReadOnly))
			iconSvg_ = file.readAll();
		setText(QString());
		setAccessibleName(accessibleName);
		setToolTip(accessibleName);
		setFixedSize(24, 24);
		setAutoRaise(true);
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::StrongFocus);
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		QToolButton::paintEvent(event);
		if (iconSvg_.isEmpty())
			return;

		QByteArray themedSvg = iconSvg_;
		const QPalette::ColorRole iconRole = property("interactionEnabled").isValid()
			? (property("interactionEnabled").toBool() ? QPalette::ButtonText :
			   (property("selected").toBool() ? QPalette::HighlightedText : QPalette::ButtonText))
			: (property("selected").toBool() ? QPalette::HighlightedText : QPalette::Link);
		themedSvg.replace("currentColor", palette().color(iconRole).name(QColor::HexRgb).toUtf8());
		QSvgRenderer renderer(themedSvg);
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const qreal iconSize = 18.0;
		renderer.render(&painter, QRectF((width() - iconSize) / 2.0, (height() - iconSize) / 2.0, iconSize, iconSize));
	}

private:
	QString iconPath_;
	QByteArray iconSvg_;
};

struct SceneLayoutCaptureContext {
	std::vector<SceneItemTransform> *items;
	std::string path;
};

struct InteractiveTriggerSearchContext {
	const OBSWorkspaceManager *manager;
	std::string workspaceId;
	std::string sourceName;
};

static bool FindInteractiveTriggerItem(obs_scene_t *scene, obs_sceneitem_t *item, void *opaque)
{
	auto *context = static_cast<InteractiveTriggerSearchContext *>(opaque);
	if (!context || !obs_sceneitem_visible(item))
		return true;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	const std::string logicalName = OBSWorkspaceManager::GetLogicalSourceName(obs_source_get_name(source));
	if (context->manager->GetSourceCategory(context->workspaceId, logicalName) == 1) {
		context->sourceName = logicalName;
		return false;
	}

	bool keepSearching = true;
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, FindInteractiveTriggerItem, context);
		keepSearching = context->sourceName.empty();
	} else if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
		obs_scene_t *nestedScene = obs_scene_from_source(source);
		if (nestedScene) {
			obs_scene_enum_items(nestedScene, FindInteractiveTriggerItem, context);
			keepSearching = context->sourceName.empty();
		}
	}

	UNUSED_PARAMETER(scene);
	return keepSearching;
}

static std::string FindInteractiveTriggerSource(const OBSWorkspaceManager *manager,
							const OBSWorkspace &workspace,
							const std::string &sceneName)
{
	const std::string aspect = OBSWorkspaceManager::GetAspectRatioKey(
		workspace.video.base_cx, workspace.video.base_cy);
	const std::string backingName = OBSWorkspaceManager::GetBackingSceneName(
		sceneName, aspect, workspace.id, workspace.video.base_cx, workspace.video.base_cy);
	obs_source_t *sceneSource = obs_get_source_by_name(backingName.c_str());
	if (!sceneSource)
		return {};

	std::string sourceName;
	if (obs_scene_t *scene = obs_scene_from_source(sceneSource)) {
		InteractiveTriggerSearchContext context{manager, workspace.id, {}};
		obs_scene_enum_items(scene, FindInteractiveTriggerItem, &context);
		sourceName = std::move(context.sourceName);
	}
	obs_source_release(sceneSource);
	return sourceName;
}

static bool CaptureSceneItemLayout(obs_scene_t *scene, obs_sceneitem_t *item, void *opaque)
{
	auto *context = static_cast<SceneLayoutCaptureContext *>(opaque);
	const int64_t itemId = obs_sceneitem_get_id(item);
	const std::string path = context->path + "/" + std::to_string(itemId);
	// The scene item returns a borrowed pointer. Own a reference while capturing.
	obs_source_t *source = obs_source_get_ref(obs_sceneitem_get_source(item));
	if (!source)
		return true;

	SceneItemTransform saved;
	saved.id = itemId;
	saved.name = path + "|" + OBSWorkspaceManager::GetLogicalSourceName(obs_source_get_name(source));
	obs_transform_info info{};
	obs_sceneitem_get_info2(item, &info);
	saved.pos_x = info.pos.x;
	saved.pos_y = info.pos.y;
	saved.scale_x = info.scale.x;
	saved.scale_y = info.scale.y;
	saved.rot = info.rot;
	saved.alignment = info.alignment;
	saved.bounds_type = info.bounds_type;
	saved.bounds_alignment = info.bounds_alignment;
	saved.bounds_x = info.bounds.x;
	saved.bounds_y = info.bounds.y;
	saved.crop_to_bounds = info.crop_to_bounds;
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_crop(item, &crop);
	saved.crop_left = crop.left;
	saved.crop_top = crop.top;
	saved.crop_right = crop.right;
	saved.crop_bottom = crop.bottom;
	saved.visible = obs_sceneitem_visible(item);
	context->items->push_back(std::move(saved));

	SceneLayoutCaptureContext childContext{context->items, path};
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, CaptureSceneItemLayout, &childContext);
	} else if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
		if (obs_scene_t *nested = obs_scene_from_source(source))
			obs_scene_enum_items(nested, CaptureSceneItemLayout, &childContext);
	}
	obs_source_release(source);
	UNUSED_PARAMETER(scene);
	return true;
}

}

class SceneDragHandle : public QLabel {
public:
	explicit SceneDragHandle(const QString &sceneName, QWidget *parent = nullptr)
		: QLabel("🎬", parent), sceneName_(sceneName)
	{
		setCursor(Qt::OpenHandCursor);
		setToolTip("انقر واسحب هنا لإعادة ترتيب المشهد لأعلى أو لأسفل");
		setObjectName("sceneDragHandle");
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			dragStartPos_ = event->pos();
			setCursor(Qt::ClosedHandCursor);
		}
		QLabel::mousePressEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		setCursor(Qt::OpenHandCursor);
		QLabel::mouseReleaseEvent(event);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!(event->buttons() & Qt::LeftButton))
			return;
		if ((event->pos() - dragStartPos_).manhattanLength() < QApplication::startDragDistance())
			return;

		auto *drag = new QDrag(this);
		auto *mime = new QMimeData();
		mime->setData("application/x-obs-workspace-scene", sceneName_.toUtf8());
		drag->setMimeData(mime);

		QPixmap pixmap(140, 26);
		const QPalette palette = QApplication::palette(this);
		pixmap.fill(palette.color(QPalette::Highlight));
		QPainter p(&pixmap);
		p.setPen(palette.color(QPalette::HighlightedText));
		QFont font = p.font();
		font.setBold(true);
		p.setFont(font);
		p.drawText(pixmap.rect(), Qt::AlignCenter, "🎬 " + sceneName_);
		p.end();

		drag->setPixmap(pixmap);
		drag->setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));

		setCursor(Qt::ClosedHandCursor);
		drag->exec(Qt::MoveAction);
		setCursor(Qt::OpenHandCursor);
	}

private:
	QString sceneName_;
	QPoint dragStartPos_;
};

class WorkspaceSceneList : public QListWidget {
public:
	explicit WorkspaceSceneList(QWidget *parent = nullptr) : QListWidget(parent)
	{
		setAcceptDrops(true);
	}

	std::function<void(int fromRow, int toRow)> onMoveScene;

protected:
	void dragEnterEvent(QDragEnterEvent *event) override
	{
		if (event->mimeData()->hasFormat("application/x-obs-workspace-scene")) {
			event->acceptProposedAction();
		} else {
			QListWidget::dragEnterEvent(event);
		}
	}

	void dragMoveEvent(QDragMoveEvent *event) override
	{
		if (event->mimeData()->hasFormat("application/x-obs-workspace-scene")) {
			event->acceptProposedAction();
		} else {
			QListWidget::dragMoveEvent(event);
		}
	}

	void dropEvent(QDropEvent *event) override
	{
		if (event->mimeData()->hasFormat("application/x-obs-workspace-scene")) {
			QString sceneName = QString::fromUtf8(event->mimeData()->data("application/x-obs-workspace-scene"));
			QListWidgetItem *targetItem = itemAt(event->position().toPoint());
			int targetRow = targetItem ? row(targetItem) : count() - 1;
			if (targetRow < 0) targetRow = 0;

			int srcRow = -1;
			for (int i = 0; i < count(); ++i) {
				if (item(i)->data(Qt::UserRole).toString() == sceneName) {
					srcRow = i;
					break;
				}
			}

			if (srcRow != -1 && onMoveScene) {
				onMoveScene(srcRow, targetRow);
			}
			event->acceptProposedAction();
		} else {
			QListWidget::dropEvent(event);
		}
	}
};

static void frontend_event_handler(enum obs_frontend_event event, void *private_data)
{
	auto *panel = static_cast<WorkspacePanel *>(private_data);
	if (!panel) return;

	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		QMetaObject::invokeMethod(panel, "OnOBSSceneChanged", Qt::QueuedConnection);
	}
}

WorkspacePanel::WorkspacePanel(OBSWorkspaceManager *mgr, OBSBasic *main, QWidget *parent)
	: QWidget(parent), mgr_(mgr), main_(main)
{
	static const int cairoFontId = QFontDatabase::addApplicationFont(":/res/fonts/Cairo-Variable.ttf");
	Q_UNUSED(cairoFontId);
	setObjectName("WorkspacePanel");
	BuildUI();
	ApplyStyles();
	RefreshAll();

	obs_frontend_add_event_callback(frontend_event_handler, this);

	// The active workspace's saved scene name is logical (for example, "بث مباشر").
	// Resolve it to the backing scene for the workspace's current resolution on startup,
	// without reapplying workspace role or text-color settings during startup.
	ActivateCurrentWorkspaceScene();
}

WorkspacePanel::~WorkspacePanel()
{
	obs_frontend_remove_event_callback(frontend_event_handler, this);
}

void WorkspacePanel::SetWorkspaceManager(OBSWorkspaceManager *mgr)
{
	mgr_ = mgr;
	RefreshAll();
}

void WorkspacePanel::BuildUI()
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(6, 6, 6, 6);
	mainLayout->setSpacing(8);

	// 1. Header & Workspace Selector Bar
	auto *wsBarWidget = new QWidget(this);
	wsBarWidget->setObjectName("wsBarWidget");
	wsBarWidget->setLayoutDirection(Qt::RightToLeft);
	auto *wsBarLayout = new QVBoxLayout(wsBarWidget);
	wsBarLayout->setContentsMargins(6, 6, 6, 6);
	wsBarLayout->setSpacing(6);

	auto *titleRow = new QHBoxLayout();
	// Keep the workspace selector on the physical right side of the bar.
	titleRow->setDirection(QBoxLayout::LeftToRight);
	comboWorkspace_ = new QComboBox(wsBarWidget);
	comboWorkspace_->setObjectName("comboWorkspace");
	comboWorkspace_->setLayoutDirection(Qt::RightToLeft);
	comboWorkspace_->setToolTip("اختيار بيئة العمل");
	comboWorkspace_->setFixedHeight(28);
	comboWorkspace_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	connect(comboWorkspace_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (index >= 0)
			OnWorkspaceSelected(comboWorkspace_->itemData(index).toString());
	});

	btnDuplicateWs_ = new QPushButton("📋 تكرار البيئة", wsBarWidget);
	btnDuplicateWs_->setObjectName("btnDuplicateWs");
	btnDuplicateWs_->setToolTip("تكرار بيئة العمل الحالية بجميع مشاهدها وإعداداتها لإنشاء بيئة جديدة");
	btnDuplicateWs_->setFixedHeight(28);
	btnDuplicateWs_->setCursor(Qt::PointingHandCursor);
	connect(btnDuplicateWs_, &QPushButton::clicked, this, &WorkspacePanel::OnDuplicateWorkspace);

	auto *interactiveModeWidget = new QWidget(wsBarWidget);
	interactiveModeWidget->setObjectName("interactiveModeWidget");
	interactiveModeWidget->setLayoutDirection(Qt::RightToLeft);
	auto *interactiveModeLayout = new QHBoxLayout(interactiveModeWidget);
	interactiveModeLayout->setContentsMargins(0, 0, 0, 0);
	interactiveModeLayout->setSpacing(6);
	lblInteractiveMode_ = new QLabel("تفاعلي", interactiveModeWidget);
	lblInteractiveMode_->setObjectName("lblInteractiveMode");
	interactiveModeToggle_ = new idian::ToggleSwitch(interactiveModeWidget);
	interactiveModeToggle_->setObjectName("interactiveModeToggle");
	interactiveModeToggle_->setAccessibleName("الوضع التفاعلي");
	interactiveModeToggle_->setAccessibleDescription("تشغيل أو إيقاف التفاعل للمشاهد التفاعلية في البيئة الحالية");
	interactiveModeToggle_->setToolTip("تشغيل أو إيقاف الوضع التفاعلي للبيئة الحالية");
	interactiveModeLayout->addWidget(lblInteractiveMode_);
	interactiveModeLayout->addWidget(interactiveModeToggle_);
	titleRow->addWidget(btnDuplicateWs_);
	titleRow->addWidget(interactiveModeWidget);
	connect(interactiveModeToggle_, &QAbstractButton::toggled,
		this, &WorkspacePanel::OnInteractiveModeToggled);
	titleRow->addWidget(comboWorkspace_, 1);
	wsBarLayout->addLayout(titleRow);

	mainLayout->addWidget(wsBarWidget);

	// 2. Resolution & Screen Dimensions Selector (تبديل مقاسات الشاشة)
	auto *resBox = new QWidget(this);
	resBox->setObjectName("resBox");
	auto *resLayout = new QHBoxLayout(resBox);
	resLayout->setContentsMargins(4, 4, 4, 4);
	resLayout->setSpacing(8);

	auto *lblRes = new QLabel("📐 مقاس الشاشة:", resBox);
	lblRes->setObjectName("lblResolutionTitle");
	resLayout->addWidget(lblRes);

	comboRes_ = new QComboBox(resBox);
	comboRes_->setObjectName("comboRes");
	comboRes_->setIconSize(QSize(22, 22));
	comboRes_->addItem(RecordingResolutionDialog::MakeAspectRatioIcon(16, 9, 22), "1920 × 1080 (Full HD عريض 16:9)", QPoint(1920, 1080));
	comboRes_->addItem(RecordingResolutionDialog::MakeAspectRatioIcon(9, 16, 22), "1080 × 1920 (Full HD طولي 9:16)", QPoint(1080, 1920));
	comboRes_->addItem(RecordingResolutionDialog::MakeAspectRatioIcon(1, 1, 22), "1080 × 1080 (مربع Square 1:1)", QPoint(1080, 1080));
	comboRes_->setCursor(Qt::PointingHandCursor);

	connect(comboRes_, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &WorkspacePanel::OnResolutionChanged);

	resLayout->addWidget(comboRes_, 1);
	mainLayout->addWidget(resBox);

	// 3. Scenes Section for Current Workspace
	auto *sceneBox = new QWidget(this);
	sceneBox->setObjectName("sceneBox");
	auto *sceneBoxLayout = new QVBoxLayout(sceneBox);
	sceneBoxLayout->setContentsMargins(0, 4, 0, 0);
	sceneBoxLayout->setSpacing(4);

	auto *sceneHeaderLayout = new QHBoxLayout();
	auto *sceneTitle = new QLabel("المشاهد في البيئة الحالية", sceneBox);
	sceneTitle->setObjectName("sectionHeader");
	sceneTitle->setLayoutDirection(Qt::RightToLeft);
	sceneHeaderLayout->addWidget(sceneTitle);
	sceneHeaderLayout->addStretch();

	btnToggleHidden_ = new QPushButton("👁️ إظهار المخفية", sceneBox);
	btnToggleHidden_->setObjectName("btnToggleHidden");
	btnToggleHidden_->setCheckable(true);
	btnToggleHidden_->setChecked(false);
	btnToggleHidden_->setFixedHeight(28);
	btnToggleHidden_->setCursor(Qt::PointingHandCursor);
	btnToggleHidden_->setToolTip("تبديل إظهار المشاهد المخفية لإلغاء إخفائها");
	connect(btnToggleHidden_, &QPushButton::clicked, this, &WorkspacePanel::OnToggleShowHidden);
	sceneHeaderLayout->addWidget(btnToggleHidden_);
	sceneBoxLayout->addLayout(sceneHeaderLayout);

	auto *customList = new WorkspaceSceneList(sceneBox);
	sceneList_ = customList;
	sceneList_->setObjectName("wsSceneList");
	sceneList_->setLayoutDirection(Qt::RightToLeft);
	sceneList_->setSelectionMode(QAbstractItemView::SingleSelection);
	connect(sceneList_, &QListWidget::itemClicked, this, &WorkspacePanel::OnSceneClicked);

	customList->onMoveScene = [this](int from, int to) {
		OnReorderScene(from, to);
	};

	sceneBoxLayout->addWidget(sceneList_, 1);

	// Scene Action Buttons Bar (➕ إضافة, 🗑️ حذف, ♻️ سلة المحذوفات)
	auto *sceneActionsLayout = new QHBoxLayout();
	sceneActionsLayout->setSpacing(6);

	btnAddScene_ = new QPushButton("➕ إضافة مشهد", sceneBox);
	btnAddScene_->setObjectName("btnAddScene");
	btnAddScene_->setFixedHeight(28);
	btnAddScene_->setCursor(Qt::PointingHandCursor);
	connect(btnAddScene_, &QPushButton::clicked, this, &WorkspacePanel::OnAddScene);
	sceneActionsLayout->addWidget(btnAddScene_, 2);

	btnRemoveScene_ = new QPushButton("🗑️ حذف", sceneBox);
	btnRemoveScene_->setObjectName("btnRemoveScene");
	btnRemoveScene_->setFixedHeight(28);
	btnRemoveScene_->setCursor(Qt::PointingHandCursor);
	connect(btnRemoveScene_, &QPushButton::clicked, this, &WorkspacePanel::OnRemoveScene);
	sceneActionsLayout->addWidget(btnRemoveScene_, 1);

	btnRecycleBin_ = new QPushButton("♻️ المحذوفات", sceneBox);
	btnRecycleBin_->setObjectName("btnRecycleBin");
	btnRecycleBin_->setToolTip("استعادة المشاهد والعناصر المحذوفة (المحتفظ بها لمدة 24 ساعة)");
	btnRecycleBin_->setFixedHeight(28);
	btnRecycleBin_->setCursor(Qt::PointingHandCursor);
	connect(btnRecycleBin_, &QPushButton::clicked, this, &WorkspacePanel::OnOpenRecycleBin);
	sceneActionsLayout->addWidget(btnRecycleBin_, 2);

	sceneBoxLayout->addLayout(sceneActionsLayout);

	mainLayout->addWidget(sceneBox, 1);
}

void WorkspacePanel::ApplyStyles()
{
	// Keep appearance in the selected theme. Local QSS here used a separate
	// blue-gray palette and overrode every light/dark theme setting.
	setStyleSheet(QString());
}

void WorkspacePanel::RefreshAll()
{
	UpdateWorkspaceButtons();
	UpdateResolutionDisplay();
	UpdateSceneList();
	UpdateSmartDirectorConfig();
}

void WorkspacePanel::UpdateWorkspaceButtons()
{
	if (!mgr_ || !comboWorkspace_) return;

	const auto &workspaces = mgr_->GetWorkspaces();
	const OBSWorkspace *active = mgr_->GetActive();
	std::string active_id = active ? active->id : "";

	comboWorkspace_->blockSignals(true);
	comboWorkspace_->clear();
	int activeIndex = -1;

	for (const auto &ws : workspaces) {
		QString id = QString::fromStdString(ws.id);
		QString name = QString::fromStdString(ws.name);

		QString iconStr = "📁";
		if (ws.id == "ws_live")   iconStr = "📡";
		else if (ws.id == "ws_lesson") iconStr = "📚";
		else if (ws.id == "ws_misc")   iconStr = "🎛️";

		const int index = comboWorkspace_->count();
		comboWorkspace_->addItem(name + "  " + iconStr, id);
		if (ws.id == active_id)
			activeIndex = index;
	}

	if (activeIndex >= 0)
		comboWorkspace_->setCurrentIndex(activeIndex);
	comboWorkspace_->blockSignals(false);
}

void WorkspacePanel::UpdateResolutionDisplay()
{
	if (!mgr_ || !comboRes_) return;

	const OBSWorkspace *active = mgr_->GetActive();
	if (!active) return;
	if (interactiveModeToggle_) {
		QSignalBlocker blocker(interactiveModeToggle_);
		interactiveModeToggle_->setStatus(active->interactive_mode_enabled);
		interactiveModeToggle_->setPending(active->interactive_mode_enabled);
	}

	// Sync combo box without triggering signal
	comboRes_->blockSignals(true);
	int foundIdx = -1;
	for (int i = 0; i < comboRes_->count(); ++i) {
		QPoint pt = comboRes_->itemData(i).toPoint();
		if ((uint32_t)pt.x() == active->video.base_cx && (uint32_t)pt.y() == active->video.base_cy) {
			foundIdx = i;
			break;
		}
	}
	if (foundIdx < 0) {
		const double currentAspect = active->video.base_cy
			? static_cast<double>(active->video.base_cx) / active->video.base_cy : 16.0 / 9.0;
		double bestDistance = std::numeric_limits<double>::max();
		for (int i = 0; i < comboRes_->count(); ++i) {
			const QPoint pt = comboRes_->itemData(i).toPoint();
			const double optionAspect = static_cast<double>(pt.x()) / pt.y();
			const double distance = std::abs(currentAspect - optionAspect);
			if (distance < bestDistance) {
				bestDistance = distance;
				foundIdx = i;
			}
		}
	}
	comboRes_->setCurrentIndex(std::max(0, foundIdx));
	comboRes_->blockSignals(false);
}

void WorkspacePanel::OnInteractiveModeToggled(bool enabled)
{
	if (!mgr_ || !interactiveModeToggle_)
		return;
	const OBSWorkspace *active = mgr_->GetActive();
	if (!active)
		return;
	OBSWorkspace *workspace = mgr_->GetById(active->id);
	if (!workspace)
		return;

	const bool previous = workspace->interactive_mode_enabled;
	workspace->interactive_mode_enabled = enabled;
	if (!mgr_->Save()) {
		workspace->interactive_mode_enabled = previous;
		QSignalBlocker blocker(interactiveModeToggle_);
		interactiveModeToggle_->setStatus(previous);
		interactiveModeToggle_->setPending(previous);
		return;
	}

	UpdateSmartDirectorConfig();
	if (enabled) {
		OBSSourceAutoRelease current = obs_frontend_get_current_scene();
		if (current)
			SmartDirector::Instance().SelectScene(
				OBSWorkspaceManager::GetLogicalSceneName(obs_source_get_name(current)));
	}
}

static bool IsAspectKey(const std::string &str)
{
	if (str == "16:9" || str == "9:16" || str == "1:1" || str == "4:3" || str == "4:5" || str == "16:10" || str == "21:9") {
		return true;
	}
	size_t colon = str.find(':');
	if (colon != std::string::npos && colon > 0 && colon < str.size() - 1) {
		for (size_t i = 0; i < str.size(); ++i) {
			if (i == colon) continue;
			if (!isdigit(static_cast<unsigned char>(str[i]))) return false;
		}
		return true;
	}
	return false;
}

static std::string GetLogicalSceneName(const std::string &name)
{
	return OBSWorkspaceManager::GetLogicalSceneName(name);
}

static std::string GetOrCreateBackingScene(const std::string &logicalName, const std::string &aspect_key,
							   const std::string &workspace_id, uint32_t cx, uint32_t cy)
{
	return OBSWorkspaceManager::GetOrCreateBackingScene(logicalName, aspect_key, workspace_id, cx, cy);
}

void WorkspacePanel::ActivateCurrentWorkspaceScene()
{
	if (!mgr_) return;
	const OBSWorkspace *active = mgr_->GetActive();
	if (!active) return;

	const std::string logicalName = !active->active_scene.empty()
		? active->active_scene
		: (!active->scenes.empty() ? active->scenes.front() : std::string());
	if (logicalName.empty()) return;

	const std::string aspectKey = OBSWorkspaceManager::GetAspectRatioKey(
		active->video.base_cx, active->video.base_cy);
	const std::string backingName = GetOrCreateBackingScene(logicalName, aspectKey, active->id,
		active->video.base_cx, active->video.base_cy);
	obs_source_t *sceneSrc = obs_get_source_by_name(backingName.c_str());
	if (sceneSrc) {
		obs_frontend_set_current_scene(sceneSrc);
		obs_source_release(sceneSrc);
		ApplyCurrentSceneLayout(aspectKey);
	}
}

void WorkspacePanel::CaptureCurrentSceneLayout(const std::string &aspect_key)
{
	if (!mgr_ || aspect_key.empty())
		return;
	const OBSWorkspace *active = mgr_->GetActive();
	if (!active)
		return;
	if (aspect_key != OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx, active->video.base_cy))
		return;
	obs_source_t *source = obs_frontend_get_current_scene();
	if (!source)
		return;
	const std::string logicalScene = GetLogicalSceneName(obs_source_get_name(source));
	const std::string expectedScene = OBSWorkspaceManager::GetBackingSceneName(
		logicalScene, aspect_key, active->id, active->video.base_cx, active->video.base_cy);
	if (expectedScene != obs_source_get_name(source)) {
		obs_source_release(source);
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(source);
	std::vector<SceneItemTransform> items;
	if (scene) {
		SceneLayoutCaptureContext context{&items, ""};
		obs_scene_enum_items(scene, CaptureSceneItemLayout, &context);
	}
	obs_source_release(source);
	if (!logicalScene.empty())
		mgr_->SaveSceneLayout(active->id, logicalScene, aspect_key, items);
}

void WorkspacePanel::CaptureActiveSceneLayout()
{
	if (!mgr_)
		return;
	const OBSWorkspace *active = mgr_->GetActive();
	if (!active)
		return;
	CaptureCurrentSceneLayout(OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx,
												 active->video.base_cy));
}

void WorkspacePanel::ApplyCurrentSceneLayout(const std::string &aspect_key)
{
	if (!mgr_ || aspect_key.empty())
		return;
	const OBSWorkspace *active = mgr_->GetActive();
	if (!active)
		return;
	obs_source_t *source = obs_frontend_get_current_scene();
	if (!source)
		return;
	const std::string logicalScene = GetLogicalSceneName(obs_source_get_name(source));
	const std::string expectedScene = OBSWorkspaceManager::GetBackingSceneName(
		logicalScene, aspect_key, active->id, active->video.base_cx, active->video.base_cy);
	if (expectedScene != obs_source_get_name(source)) {
		obs_source_release(source);
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(source);
	// The exact-size scene owns its saved positions, crops and item processing.
	// Workspace snapshots are recovery data, not newer than the scene's edits.
	// Replaying them here silently overwrote changes made before closing HK.
	if (scene)
		obs_scene_set_custom_size(scene, true, active->video.base_cx, active->video.base_cy);
	obs_source_release(source);
}

void WorkspacePanel::ApplySceneLayout(const std::string &aspect_key)
{
	ApplyCurrentSceneLayout(aspect_key);
}

void WorkspacePanel::OnResolutionChanged(int index)
{
	if (!mgr_ || index < 0) return;
	OBSBasic::InstantSceneChangeScope instantSceneChange(main_);

	const OBSWorkspace *active = mgr_->GetActive();
	if (!active) return;

	QPoint pt = comboRes_->itemData(index).toPoint();
	uint32_t cx = (uint32_t)pt.x();
	uint32_t cy = (uint32_t)pt.y();

	std::string new_aspect = OBSWorkspaceManager::GetAspectRatioKey(cx, cy);

	// Get current active scene and aspect key BEFORE switching
	obs_source_t *cur_src = obs_frontend_get_current_scene();
	std::string cur_obs_name = cur_src ? obs_source_get_name(cur_src) : "";
	if (cur_src) obs_source_release(cur_src);

	std::string cur_logical_name = GetLogicalSceneName(cur_obs_name);
	if (cur_logical_name.empty() && !active->active_scene.empty())
		cur_logical_name = active->active_scene;
	if (cur_logical_name.empty() && !active->scenes.empty())
		cur_logical_name = active->scenes[0];
	const std::string old_aspect = OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx,
												 active->video.base_cy);
	CaptureCurrentSceneLayout(old_aspect);

	mgr_->SetWorkspaceResolution(active->id, cx, cy);

	if (main_)
		main_->ApplyWorkspaceVideo(*active);

	// Each backing scene owns its scene-item transforms. Let OBS retain those
	// transforms on the scene items instead of replaying source-level snapshots.
	if (!cur_logical_name.empty()) {
		std::string backingName = GetOrCreateBackingScene(cur_logical_name, new_aspect, active->id, cx, cy);
		obs_source_t *targetSrc = obs_get_source_by_name(backingName.c_str());
		if (targetSrc) {
			obs_frontend_set_current_scene(targetSrc);
			obs_source_release(targetSrc);
			ApplyCurrentSceneLayout(new_aspect);
		}
		mgr_->SetActiveScene(active->id, cur_logical_name);
	}

	UpdateResolutionDisplay();
	UpdateSceneList();
	mgr_->Save();
}




void WorkspacePanel::UpdateSceneList()
{
	if (!mgr_ || !sceneList_) return;

	updatingSelection_ = true;
	sceneList_->clear();

	const OBSWorkspace *active = mgr_->GetActive();
	if (!active) {
		updatingSelection_ = false;
		return;
	}

	// Current OBS active scene
	obs_source_t *cur_src = obs_frontend_get_current_scene();
	std::string current_obs_scene_name = cur_src ? obs_source_get_name(cur_src) : "";
	if (cur_src) obs_source_release(cur_src);

	std::string current_logical_name = GetLogicalSceneName(current_obs_scene_name);

	QListWidgetItem *itemToSelect = nullptr;

	std::string current_aspect_key = OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx, active->video.base_cy);
	for (const auto &scene_name : active->scenes) {
		bool isHidden = mgr_->IsSceneHidden(active->id, scene_name, current_aspect_key);
		if (isHidden && !showHiddenScenes_) {
			continue;
		}

		QString qname = QString::fromStdString(scene_name);
		auto *item = new QListWidgetItem(sceneList_);
		item->setData(Qt::UserRole, qname);

		auto *rowWidget = new QWidget();
		rowWidget->setObjectName("sceneRow");
		rowWidget->setLayoutDirection(Qt::RightToLeft);
		auto *rowLayout = new QHBoxLayout(rowWidget);
		rowLayout->setContentsMargins(6, 3, 6, 3);
		rowLayout->setSpacing(6);

		// Drag handle icon on the right (صورة الفيديو للأمساك وإعادة الترتيب)
		auto *dragHandle = new SceneDragHandle(qname, rowWidget);

		auto *lblName = new QLabel(qname, rowWidget);
		lblName->setObjectName("sceneName");
		lblName->setProperty("hidden", isHidden);
		lblName->setAttribute(Qt::WA_TransparentForMouseEvents);

		rowLayout->addWidget(dragHandle, 0, Qt::AlignVCenter);
		rowLayout->addWidget(lblName, 0, Qt::AlignVCenter);

		if (isHidden) {
			auto *lblHiddenBadge = new QLabel("🔒 مخفي", rowWidget);
			lblHiddenBadge->setObjectName("sceneHiddenBadge");
			lblHiddenBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
			rowLayout->addWidget(lblHiddenBadge, 0, Qt::AlignVCenter);
		}

		// The base scene remains fixed; other scenes can independently be marked
		// as interactive, so multiple source-to-scene pairs can coexist.
		if (mgr_->IsBaseScene(active->id, scene_name)) {
			auto *lblBadge = new QLabel("🏠 أساسي", rowWidget);
			lblBadge->setObjectName("sceneRoleBadge");
			lblBadge->setProperty("role", "base");
			lblBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
			rowLayout->addWidget(lblBadge, 0, Qt::AlignVCenter);
		} else {
			const bool isInteractive = mgr_->IsInteractiveScene(active->id, scene_name);
			auto *interactiveCheck = new QCheckBox(rowWidget);
			interactiveCheck->setObjectName("sceneInteractiveCheck");
			interactiveCheck->setChecked(isInteractive);
			interactiveCheck->setAccessibleName(QString("المشهد التفاعلي %1").arg(qname));
			interactiveCheck->setToolTip(isInteractive
				? "إلغاء ربط هذا المشهد بالوضع التفاعلي"
				: "تعيين هذا المشهد تفاعليًا باستخدام المشهد الأساسي كبديل");
			connect(interactiveCheck, &QCheckBox::toggled, this, [this, scene_name](bool checked) {
				if (!mgr_) return;
				const OBSWorkspace *workspace = mgr_->GetActive();
				if (!workspace) return;
				if (checked) {
					const std::string baseScene = mgr_->GetBaseScene(workspace->id);
					if (baseScene.empty() || baseScene == scene_name) {
						QMessageBox::warning(this, QString::fromUtf8("المشهد الأساسي"),
							QString::fromUtf8("حدد المشهد الأساسي لبيئة العمل قبل تعيين المشاهد التفاعلية."));
						UpdateSceneList();
						return;
					}

					std::string triggerSource = mgr_->GetInteractiveSource(workspace->id, scene_name);
					if (triggerSource.empty())
						triggerSource = FindInteractiveTriggerSource(mgr_, *workspace, scene_name);
					if (triggerSource.empty()) {
						QMessageBox::warning(this, QString::fromUtf8("المصدر التفاعلي"),
							QString::fromUtf8("لا يوجد مصدر متغير ظاهر داخل هذا المشهد لربطه بالتفاعل."));
						UpdateSceneList();
						return;
					}

					const std::string conflict = mgr_->FindConflictSceneForSource(
						workspace->id, triggerSource, scene_name);
					if (!conflict.empty()) {
						QMessageBox::warning(this, QString::fromUtf8("المصدر التفاعلي"),
							QString::fromUtf8("المصدر «%1» مرتبط مسبقًا بالمشهد «%2».")
								.arg(QString::fromStdString(triggerSource), QString::fromStdString(conflict)));
						UpdateSceneList();
						return;
					}

					mgr_->SetInteractiveScene(workspace->id, scene_name, triggerSource, baseScene);
					if (!mgr_->IsInteractiveScene(workspace->id, scene_name)) {
						QMessageBox::warning(this, QString::fromUtf8("حفظ المشهد"),
							QString::fromUtf8("تعذر حفظ إعداد المشهد التفاعلي."));
					}
					UpdateSmartDirectorConfig();
					UpdateSceneList();
					return;
				}
				if (mgr_->IsInteractiveScene(workspace->id, scene_name)) {
					if (mgr_->ClearSceneRole(workspace->id, scene_name)) {
						UpdateSmartDirectorConfig();
					} else {
						QMessageBox::warning(this, QString::fromUtf8("حفظ المشهد"),
							QString::fromUtf8("تعذر حفظ تغيير دور المشهد."));
					}
				}
				UpdateSceneList();
			});
			rowLayout->addWidget(interactiveCheck, 0, Qt::AlignVCenter);
		}

		rowLayout->addStretch(1);

		// Eye button to hide / unhide scene
		auto *btnEye = new SceneActionButton(
			isHidden ? ":/res/images/lucide/eye-off.svg" : ":/res/images/lucide/eye.svg",
			isHidden ? "إظهار المشهد" : "إخفاء المشهد", rowWidget);
		btnEye->setObjectName("sceneVisibilityButton");
		btnEye->setProperty("hidden", isHidden);

		connect(btnEye, &QPushButton::clicked, this, [this, scene_name]() {
			if (!mgr_ || !mgr_->GetActive()) return;
			const OBSWorkspace *act = mgr_->GetActive();
			std::string asp_key = OBSWorkspaceManager::GetAspectRatioKey(act->video.base_cx, act->video.base_cy);
			mgr_->ToggleSceneHidden(act->id, scene_name, asp_key);
			mgr_->Save();
			UpdateSceneList();
		});
		rowLayout->addWidget(btnEye, 0, Qt::AlignVCenter);

		// The item owns the full row height; badges stay centered at their text height.
		const int rowHeight = std::max(32, rowWidget->sizeHint().height());
		rowWidget->setMinimumHeight(rowHeight);
		item->setSizeHint(QSize(0, rowHeight));
		sceneList_->setItemWidget(item, rowWidget);

		if (scene_name == current_logical_name) {
			itemToSelect = item;
		} else if (!itemToSelect && scene_name == active->active_scene) {
			itemToSelect = item;
		}
	}

	if (!itemToSelect && sceneList_->count() > 0) {
		itemToSelect = sceneList_->item(0);
	}

	if (itemToSelect) {
		sceneList_->setCurrentItem(itemToSelect);
	}

	for (int i = 0; i < sceneList_->count(); ++i) {
		QListWidgetItem *item = sceneList_->item(i);
		QWidget *rowWidget = sceneList_->itemWidget(item);
		if (!rowWidget)
			continue;

		const bool selected = item == itemToSelect;
		rowWidget->setProperty("selected", selected);
		rowWidget->style()->unpolish(rowWidget);
		rowWidget->style()->polish(rowWidget);
		if (QLabel *label = rowWidget->findChild<QLabel *>("sceneName")) {
			label->setProperty("selected", selected);
			label->style()->unpolish(label);
			label->style()->polish(label);
		}
		for (QToolButton *action : rowWidget->findChildren<QToolButton *>()) {
			action->setProperty("selected", selected);
			action->style()->unpolish(action);
			action->style()->polish(action);
		}
	}

	updatingSelection_ = false;
}

void WorkspacePanel::OnWorkspaceSelected(const QString &id)
{
	if (!mgr_) return;
	OBSBasic::InstantSceneChangeScope instantSceneChange(main_);
	if (const OBSWorkspace *current = mgr_->GetActive()) {
		const std::string currentAspect = OBSWorkspaceManager::GetAspectRatioKey(current->video.base_cx,
												 current->video.base_cy);
		CaptureCurrentSceneLayout(currentAspect);
	}

	std::string stdId = id.toStdString();
	if (mgr_->SwitchTo(stdId)) {
		const OBSWorkspace *active = mgr_->GetActive();
		if (active && main_) {
			main_->ApplyWorkspaceVideo(*active);
			std::string aspect_key = OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx, active->video.base_cy);
			std::string logicalToActivate = active->active_scene.empty() ?
				(!active->scenes.empty() ? active->scenes[0] : "") : active->active_scene;

			if (!logicalToActivate.empty()) {
				std::string backingName = GetOrCreateBackingScene(logicalToActivate, aspect_key, active->id,
														   active->video.base_cx, active->video.base_cy);
				obs_source_t *sceneSrc = obs_get_source_by_name(backingName.c_str());
				if (sceneSrc) {
					obs_frontend_set_current_scene(sceneSrc);
					obs_source_release(sceneSrc);
					ApplyCurrentSceneLayout(aspect_key);
				}
				mgr_->ApplySceneTextColors(active->id, logicalToActivate);
			}
		}
		RefreshAll();
		OBSSourceAutoRelease current = obs_frontend_get_current_scene();
		if (current)
			SmartDirector::Instance().SelectScene(GetLogicalSceneName(obs_source_get_name(current)));
		emit workspaceChanged(id);
	}
}

void WorkspacePanel::OnSceneClicked(QListWidgetItem *item)
{
	if (!item || updatingSelection_ || !mgr_) return;

	QString logicalName = item->data(Qt::UserRole).toString();
	const OBSWorkspace *active = mgr_->GetActive();
	if (!active) return;

	std::string aspect_key = OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx, active->video.base_cy);
	CaptureCurrentSceneLayout(aspect_key);
	std::string backingName = GetOrCreateBackingScene(logicalName.toStdString(), aspect_key, active->id,
														   active->video.base_cx, active->video.base_cy);

	obs_source_t *sceneSrc = obs_get_source_by_name(backingName.c_str());
	if (sceneSrc) {
		SmartDirector::Instance().SelectScene(logicalName.toStdString());
		obs_frontend_set_current_scene(sceneSrc);
		obs_source_release(sceneSrc);
		ApplyCurrentSceneLayout(aspect_key);

		mgr_->SetActiveScene(active->id, logicalName.toStdString());
		mgr_->Save();
		mgr_->ApplySceneTextColors(active->id, logicalName.toStdString());
	}
}

void WorkspacePanel::OnOBSSceneChanged()
{
	if (main_)
		main_->RefreshInteractiveSourceSelectors();
	if (!mgr_ || updatingSceneContext_) return;
	const OBSWorkspace *active = mgr_->GetActive();
	if (!active) return;
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	if (!current) return;
	const std::string currentName = obs_source_get_name(current);
	const std::string logicalName = GetLogicalSceneName(currentName);
	if (std::find(active->scenes.begin(), active->scenes.end(), logicalName) == active->scenes.end()) return;

	QScopedValueRollback<bool> guard(updatingSceneContext_, true);
	const std::string aspect = OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx, active->video.base_cy);
	const std::string expected = OBSWorkspaceManager::GetBackingSceneName(
		logicalName, aspect, active->id, active->video.base_cx, active->video.base_cy);
	if (currentName != expected) {
		const std::string backing = GetOrCreateBackingScene(logicalName, aspect, active->id,
			active->video.base_cx, active->video.base_cy);
		OBSSourceAutoRelease target = obs_get_source_by_name(backing.c_str());
		if (!target) return;
		/* This is a correction, not something the user asked for, so do not fade. */
		OBSBasic::InstantSceneChangeScope instantSceneChange(main_);
		obs_frontend_set_current_scene(target);
		ApplyCurrentSceneLayout(aspect);
	}
	mgr_->SetActiveScene(active->id, logicalName);
	mgr_->ApplySceneTextColors(active->id, logicalName);
	if (main_)
		main_->FitTextItemsToSmartFrame();
	UpdateSceneList();
}

void WorkspacePanel::OnAddScene()
{
	if (!main_) return;

	main_->on_actionAddScene_triggered();

	obs_source_t *newCur = obs_frontend_get_current_scene();
	if (newCur && mgr_ && mgr_->GetActive()) {
		std::string newObsName = obs_source_get_name(newCur);
		std::string logicalName = GetLogicalSceneName(newObsName);

		const OBSWorkspace *active = mgr_->GetActive();
		std::string aspect_key = OBSWorkspaceManager::GetAspectRatioKey(active->video.base_cx, active->video.base_cy);

		obs_source_release(newCur);

		OBSWorkspace *ws = mgr_->GetById(active->id);
		if (ws) {
			if (std::find(ws->scenes.begin(), ws->scenes.end(), logicalName) == ws->scenes.end()) {
				ws->scenes.push_back(logicalName);
			}
			ws->active_scene = logicalName;
			mgr_->Save();
			std::string backingName = GetOrCreateBackingScene(logicalName, aspect_key, active->id,
															   active->video.base_cx, active->video.base_cy);
			obs_source_t *backingScene = obs_get_source_by_name(backingName.c_str());
			if (backingScene) {
				obs_frontend_set_current_scene(backingScene);
				obs_source_release(backingScene);
			}
		}
	}
	UpdateSceneList();
}

void WorkspacePanel::OnRemoveScene()
{
	if (!main_ || !sceneList_ || !sceneList_->currentItem()) return;

	QString logicalName = sceneList_->currentItem()->data(Qt::UserRole).toString();
	std::string stdLogical = logicalName.toStdString();

	// Backup and remove all backing scenes for this logical scene
	std::vector<std::string> aspects = {"16:9", "9:16", "1:1", "4:3", "4:5", "16:10", "21:9"};
	std::vector<std::string> sceneNamesToRemove = {stdLogical};
	for (const auto &asp : aspects) {
		sceneNamesToRemove.push_back(stdLogical + " [" + asp + "]");
	}

	for (const auto &sName : sceneNamesToRemove) {
		obs_source_t *s = obs_get_source_by_name(sName.c_str());
		if (s) {
			obs_data_t *data = obs_save_source(s);
			const char *jsonStr = obs_data_get_json(data);
			std::string wsId = (mgr_ && mgr_->GetActive()) ? mgr_->GetActive()->id : "";
			RecycleBin::Instance().AddDeletedScene(sName, wsId, jsonStr ? jsonStr : "");
			obs_data_release(data);

			obs_source_remove(s);
			obs_source_release(s);
		}
	}

	if (mgr_ && mgr_->GetActive()) {
		OBSWorkspace *ws = mgr_->GetById(mgr_->GetActive()->id);
		if (ws) {
			auto &scenes = ws->scenes;
			scenes.erase(std::remove(scenes.begin(), scenes.end(), stdLogical), scenes.end());
			ws->interactive_scenes.erase(stdLogical);
			if (ws->base_scene == stdLogical) {
				ws->base_scene.clear();
				for (const auto &candidate : scenes) {
					if (ws->interactive_scenes.find(candidate) == ws->interactive_scenes.end()) {
						ws->base_scene = candidate;
						break;
					}
				}
			}
			for (auto it = ws->interactive_scenes.begin(); it != ws->interactive_scenes.end();) {
				if (it->second.fallback_scene == stdLogical) {
					std::string replacement = ws->base_scene;
					if (replacement.empty() || replacement == it->first ||
					    ws->interactive_scenes.find(replacement) != ws->interactive_scenes.end()) {
						replacement.clear();
						for (const auto &candidate : scenes) {
							if (candidate != it->first &&
							    ws->interactive_scenes.find(candidate) == ws->interactive_scenes.end()) {
								replacement = candidate;
								break;
							}
						}
					}
					if (replacement.empty()) {
						it = ws->interactive_scenes.erase(it);
						continue;
					}
					it->second.fallback_scene = replacement;
				}
				++it;
			}
			if (ws->active_scene == stdLogical) {
				ws->active_scene = ws->scenes.empty() ? "" : ws->scenes[0];
			}
			mgr_->Save();
			UpdateSmartDirectorConfig();
		}
	}
	UpdateSceneList();
}

void WorkspacePanel::OnReorderScene(int fromRow, int toRow)
{
	if (!mgr_ || !mgr_->GetActive() || fromRow == toRow) return;

	OBSWorkspace *ws = mgr_->GetById(mgr_->GetActive()->id);
	if (!ws) return;

	auto &scenes = ws->scenes;
	if (fromRow >= 0 && fromRow < (int)scenes.size() &&
	    toRow >= 0 && toRow < (int)scenes.size()) {
		std::string movingScene = scenes[fromRow];
		scenes.erase(scenes.begin() + fromRow);
		scenes.insert(scenes.begin() + toRow, movingScene);

		mgr_->Save();
		UpdateSceneList();
		if (sceneList_) {
			sceneList_->setCurrentRow(toRow);
		}
	}
}

void WorkspacePanel::UpdateSmartDirectorConfig()
{
	if (!mgr_) return;
	const OBSWorkspace *active = mgr_->GetActive();
	if (active) {
		std::map<std::string, OBSInteractiveSceneConfig> eligibleScenes;
		for (const auto &entry : active->interactive_scenes) {
			if (mgr_->GetSourceCategory(active->id, entry.second.source_name) == 1) {
				OBSInteractiveSceneConfig config = entry.second;
				config.enabled = true;
				eligibleScenes.emplace(entry.first, std::move(config));
			}
		}
		SmartDirector::Instance().SetWorkspaceConfig(active->id, active->base_scene, eligibleScenes);
		SmartDirector::Instance().SetEnabled(active->interactive_mode_enabled);
	}
}

void WorkspacePanel::OnDuplicateWorkspace()
{
	if (!mgr_ || !mgr_->GetActive()) return;

	const OBSWorkspace *active = mgr_->GetActive();
	QString currentName = QString::fromStdString(active->name);
	bool ok = false;
	QString newName = QInputDialog::getText(this, "تكرار بيئة العمل",
		QString("أدخل اسماً للبيئة الجديدة المنسوخة من (%1):").arg(currentName),
		QLineEdit::Normal,
		currentName + " - نسخة",
		&ok);

	if (!ok || newName.trimmed().isEmpty()) {
		return;
	}

	std::string newId;
	if (mgr_->DuplicateWorkspace(active->id, newName.trimmed().toStdString(), newId)) {
		RefreshAll();
		OnWorkspaceSelected(QString::fromStdString(newId));
		QMessageBox::information(this, "تكرار البيئة",
			QString("تم إنشاء بيئة العمل الجديدة بنجاح:\n%1").arg(newName.trimmed()));
	}
}

void WorkspacePanel::OnOpenRecycleBin()
{
	RecycleBinDialog dlg(this);
	dlg.exec();
	UpdateSceneList();
}

void WorkspacePanel::OnToggleShowHidden()
{
	showHiddenScenes_ = btnToggleHidden_ ? btnToggleHidden_->isChecked() : !showHiddenScenes_;
	if (btnToggleHidden_) {
		btnToggleHidden_->setText(showHiddenScenes_ ? "👁️ إخفاء المخفية" : "👁️ إظهار المخفية");
	}
	UpdateSceneList();
}

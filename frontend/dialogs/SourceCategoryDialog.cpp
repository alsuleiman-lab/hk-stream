#include "SourceCategoryDialog.hpp"
#include "workspaces/WorkspaceModel.hpp"
#include "widgets/OBSBasic.hpp"
#include <obs.h>
#include <obs-frontend-api.h>
#include <QGroupBox>
#include <QMessageBox>
#include <algorithm>
#include <set>

SourceCategoryDialog::SourceCategoryDialog(OBSWorkspaceManager *mgr, OBSBasic *main, QWidget *parent)
	: QDialog(parent), mgr_(mgr), main_(main)
{
	setWindowTitle("⚙️ محرر مصادر بيئة العمل (الأساسية والمتغيرة)");
	setLayoutDirection(Qt::RightToLeft);
	resize(780, 520);

	buildUI();
	applyStylesheet();
	populateLists();
}

void SourceCategoryDialog::buildUI()
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(16, 14, 16, 14);
	mainLayout->setSpacing(12);

	// Header banner
	auto *headerBox = new QWidget(this);
	auto *hbLayout = new QVBoxLayout(headerBox);
	hbLayout->setContentsMargins(10, 8, 10, 8);
	hbLayout->setSpacing(4);

	lblWsName_ = new QLabel(this);
	lblWsName_->setStyleSheet("font-size: 15px; font-weight: bold; color: #63b3ed;");
	hbLayout->addWidget(lblWsName_);

	auto *lblDesc = new QLabel(
		"المصادر الأساسية: يتم قفلها وحمايتها تلقائياً أثناء البث المباشر لمنع تحريكها بالخطأ.\n"
		"المصادر المتغيرة: مصادر المحتوى التفاعلية والدروس المتاحة للتعديل المستمر أثناء البث.",
		this);
	lblDesc->setStyleSheet("font-size: 12px; color: #a0aec0; line-height: 1.4;");
	lblDesc->setWordWrap(true);
	hbLayout->addWidget(lblDesc);

	mainLayout->addWidget(headerBox);

	// Three Columns Layout:
	// Right: Core Sources | Middle: Variable Sources | Left: Available (Unassigned) Sources
	auto *colsLayout = new QHBoxLayout();
	colsLayout->setSpacing(12);

	// 1. Core Sources Box
	auto *groupCore = new QGroupBox("🛡️ المصادر الأساسية (Core)", this);
	auto *coreLayout = new QVBoxLayout(groupCore);
	coreLayout->setContentsMargins(8, 12, 8, 8);
	listCore_ = new QListWidget(groupCore);
	listCore_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	coreLayout->addWidget(listCore_);
	colsLayout->addWidget(groupCore);

	// 2. Variable Sources Box
	auto *groupVar = new QGroupBox("⚡ المصادر المتغيرة (Variable)", this);
	auto *varLayout = new QVBoxLayout(groupVar);
	varLayout->setContentsMargins(8, 12, 8, 8);
	listVariable_ = new QListWidget(groupVar);
	listVariable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	varLayout->addWidget(listVariable_);
	colsLayout->addWidget(groupVar);

	// 3. Unassigned / Available Sources Box
	auto *groupAvail = new QGroupBox("📋 مصادر أخرى غير مصنفة", this);
	auto *availLayout = new QVBoxLayout(groupAvail);
	availLayout->setContentsMargins(8, 12, 8, 8);
	listAvailable_ = new QListWidget(groupAvail);
	listAvailable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
	availLayout->addWidget(listAvailable_);
	colsLayout->addWidget(groupAvail);

	mainLayout->addLayout(colsLayout, 1);

	// Action buttons between lists
	auto *actionLayout = new QHBoxLayout();
	actionLayout->setSpacing(8);

	btnMoveToCore_ = new QPushButton("🛡️ نقل إلى الأساسية", this);
	btnMoveToCore_->setFixedHeight(30);
	btnMoveToCore_->setCursor(Qt::PointingHandCursor);
	btnMoveToCore_->setStyleSheet("background: #2b6cb0; color: white; font-weight: bold; padding: 0 14px; border-radius: 4px;");
	connect(btnMoveToCore_, &QPushButton::clicked, this, &SourceCategoryDialog::onMoveToCore);
	actionLayout->addWidget(btnMoveToCore_);

	btnMoveToVariable_ = new QPushButton("⚡ نقل إلى المتغيرة", this);
	btnMoveToVariable_->setFixedHeight(30);
	btnMoveToVariable_->setCursor(Qt::PointingHandCursor);
	btnMoveToVariable_->setStyleSheet("background: #b7791f; color: white; font-weight: bold; padding: 0 14px; border-radius: 4px;");
	connect(btnMoveToVariable_, &QPushButton::clicked, this, &SourceCategoryDialog::onMoveToVariable);
	actionLayout->addWidget(btnMoveToVariable_);

	btnRemoveCategory_ = new QPushButton("✕ إزالة التصنيف", this);
	btnRemoveCategory_->setFixedHeight(30);
	btnRemoveCategory_->setCursor(Qt::PointingHandCursor);
	btnRemoveCategory_->setStyleSheet("background: #4a5568; color: white; padding: 0 14px; border-radius: 4px;");
	connect(btnRemoveCategory_, &QPushButton::clicked, this, &SourceCategoryDialog::onRemoveCategory);
	actionLayout->addWidget(btnRemoveCategory_);

	actionLayout->addStretch();
	mainLayout->addLayout(actionLayout);

	// Bottom Save & Cancel Buttons
	auto *bottomLayout = new QHBoxLayout();
	bottomLayout->setSpacing(10);
	bottomLayout->addStretch();

	btnCancel_ = new QPushButton("إلغاء", this);
	btnCancel_->setFixedHeight(32);
	btnCancel_->setFixedWidth(90);
	btnCancel_->setCursor(Qt::PointingHandCursor);
	connect(btnCancel_, &QPushButton::clicked, this, &QDialog::reject);
	bottomLayout->addWidget(btnCancel_);

	btnSave_ = new QPushButton("💾 حفظ وتطبيق", this);
	btnSave_->setFixedHeight(32);
	btnSave_->setFixedWidth(130);
	btnSave_->setCursor(Qt::PointingHandCursor);
	btnSave_->setStyleSheet("background: #2f855a; color: white; font-weight: bold; border-radius: 4px;");
	connect(btnSave_, &QPushButton::clicked, this, &SourceCategoryDialog::onSaveClicked);
	bottomLayout->addWidget(btnSave_);

	mainLayout->addLayout(bottomLayout);
}

void SourceCategoryDialog::applyStylesheet()
{
	setStyleSheet(R"(
		QDialog {
			background-color: #171923;
			color: #e2e8f0;
			font-family: 'Segoe UI', Arial, sans-serif;
		}
		QGroupBox {
			font-size: 13px;
			font-weight: bold;
			color: #cbd5e0;
			border: 1px solid #2d3748;
			border-radius: 6px;
			margin-top: 14px;
			padding-top: 10px;
		}
		QGroupBox::title {
			subcontrol-origin: margin;
			subcontrol-position: top right;
			padding: 0 8px;
		}
		QListWidget {
			background-color: #1a202c;
			border: 1px solid #2d3748;
			border-radius: 5px;
			color: #edf2f7;
			padding: 4px;
			font-size: 12px;
		}
		QListWidget::item {
			padding: 6px 8px;
			border-radius: 4px;
			margin-bottom: 2px;
		}
		QListWidget::item:hover {
			background-color: #2d3748;
		}
		QListWidget::item:selected {
			background-color: #3182ce;
			color: #ffffff;
		}
		QPushButton {
			border: 1px solid #4a5568;
			border-radius: 4px;
			padding: 4px 12px;
			font-size: 12px;
			color: #e2e8f0;
			background-color: #2d3748;
		}
		QPushButton:hover {
			background-color: #4a5568;
		}
	)");
}

void SourceCategoryDialog::populateLists()
{
	listCore_->clear();
	listVariable_->clear();
	listAvailable_->clear();

	if (!mgr_) return;
	const OBSWorkspace *ws = mgr_->GetActive();
	if (!ws) return;

	wsId_ = ws->id;
	lblWsName_->setText(QString("بيئة العمل النشطة: [%1]").arg(QString::fromStdString(ws->name)));

	std::set<std::string> categorized;

	for (const auto &name : ws->core_sources) {
		const std::string logicalName = OBSWorkspaceManager::GetLogicalSourceName(name);
		if (categorized.insert(logicalName).second)
			listCore_->addItem(QString::fromStdString(logicalName));
	}

	for (const auto &name : ws->variable_sources) {
		const std::string logicalName = OBSWorkspaceManager::GetLogicalSourceName(name);
		if (categorized.insert(logicalName).second)
			listVariable_->addItem(QString::fromStdString(logicalName));
	}

	// Gather all scene items in the current scene and workspace scenes
	auto enumSourceCallback = [](void *data, obs_source_t *source) -> bool {
		auto *setPtr = static_cast<std::set<std::string> *>(data);
		if (source && setPtr) {
			const char *name = obs_source_get_name(source);
			if (name && *name) {
				setPtr->insert(OBSWorkspaceManager::GetLogicalSourceName(name));
			}
		}
		return true;
	};

	std::set<std::string> allSources;
	obs_enum_sources(enumSourceCallback, &allSources);

	for (const auto &srcName : allSources) {
		if (!srcName.empty() && categorized.find(srcName) == categorized.end()) {
			listAvailable_->addItem(QString::fromStdString(srcName));
		}
	}
}

void SourceCategoryDialog::onMoveToCore()
{
	// From Variable or Available to Core
	auto moveItems = [this](QListWidget *fromList) {
		QList<QListWidgetItem *> selected = fromList->selectedItems();
		for (auto *it : selected) {
			QString text = it->text();
			delete it;
			listCore_->addItem(text);
		}
	};
	moveItems(listVariable_);
	moveItems(listAvailable_);
}

void SourceCategoryDialog::onMoveToVariable()
{
	auto moveItems = [this](QListWidget *fromList) {
		QList<QListWidgetItem *> selected = fromList->selectedItems();
		for (auto *it : selected) {
			QString text = it->text();
			delete it;
			listVariable_->addItem(text);
		}
	};
	moveItems(listCore_);
	moveItems(listAvailable_);
}

void SourceCategoryDialog::onRemoveCategory()
{
	auto moveItems = [this](QListWidget *fromList) {
		QList<QListWidgetItem *> selected = fromList->selectedItems();
		for (auto *it : selected) {
			QString text = it->text();
			delete it;
			listAvailable_->addItem(text);
		}
	};
	moveItems(listCore_);
	moveItems(listVariable_);
}

void SourceCategoryDialog::onSaveClicked()
{
	if (!mgr_ || wsId_.empty()) {
		accept();
		return;
	}

	std::vector<std::string> coreVec;
	for (int i = 0; i < listCore_->count(); ++i) {
		coreVec.push_back(listCore_->item(i)->text().toStdString());
	}

	std::vector<std::string> varVec;
	for (int i = 0; i < listVariable_->count(); ++i) {
		varVec.push_back(listVariable_->item(i)->text().toStdString());
	}

	if (!mgr_->SetWorkspaceSources(wsId_, coreVec, varVec)) {
		QMessageBox::critical(this, "تعذر حفظ التصنيفات",
				      "لم يتمكن البرنامج من حفظ تصنيفات المصادر في ملف بيئة العمل. تحقق من صلاحية الكتابة ثم أعد المحاولة.");
		return;
	}

	if (main_) {
		main_->RefreshSourceCategoryState();
	}

	accept();
}

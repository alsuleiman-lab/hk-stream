#include "SceneRoleDialog.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QFrame>
#include <QCheckBox>
#include <QColorDialog>
#include <QPainter>
#include <QStyleOptionButton>
#include <QScrollArea>
#include <OBSApp.hpp>
#include <utility/platform.hpp>

#include "moc_SceneRoleDialog.cpp"

namespace {
class SceneChoiceRadio final : public QRadioButton {
public:
	using QRadioButton::QRadioButton;
protected:
	void paintEvent(QPaintEvent *event) override
	{
		QRadioButton::paintEvent(event);
		if (HighContrastEnabled() || (App()->GetTheme() && App()->GetTheme()->id.endsWith(".System"))) return;
		QStyleOptionButton option; initStyleOption(&option);
		QRectF circle = style()->subElementRect(QStyle::SE_RadioButtonIndicator, &option, this);
		circle.adjust(1, 1, -1, -1);
		QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(QPen(option.palette.color(QPalette::WindowText), 1.8));
		painter.setBrush(option.palette.color(isChecked() ? QPalette::Highlight : QPalette::Base));
		painter.drawEllipse(circle);
		if (isChecked()) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(option.palette.color(QPalette::HighlightedText));
			const qreal inset = circle.width() * .30;
			painter.drawEllipse(circle.adjusted(inset, inset, -inset, -inset));
		}
	}
};

class SceneColorButton final : public QPushButton {
public:
	using QPushButton::QPushButton;
protected:
	void paintEvent(QPaintEvent *event) override
	{
		QPushButton::paintEvent(event);
		const QColor color = property("swatchColor").value<QColor>();
		if (!color.isValid()) return;
		QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
		const qreal side = qMin(18, height() - 12);
		QRectF swatch(width() - side - 8, (height() - side) / 2, side, side);
		painter.setPen(QPen(palette().color(QPalette::ButtonText), 1.5));
		painter.setBrush(color); painter.drawRoundedRect(swatch, 3, 3);
	}
};
}

SceneRoleDialog::SceneRoleDialog(const QString &sceneName,
				 const QString &workspaceId,
				 const QString &workspaceName,
				 Role currentRole,
				 const QString &currentSource,
				 const QString &currentFallbackScene,
				 const QStringList &availableSources,
				 const QStringList &availableFallbackScenes,
				 QWidget *parent)
	: QDialog(parent),
	  sceneName_(sceneName),
	  workspaceId_(workspaceId)
{
	setWindowTitle(QString("إعدادات دور المشهد: %1").arg(sceneName));
	setModal(true);
	resize(520, 700);
	setLayoutDirection(Qt::RightToLeft);

	setObjectName("SceneRoleDialog");
	// HK's shared theme loader resolves color and font tokens.
	setStyleSheet(QString());

	auto *rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(18, 16, 18, 16);
	rootLayout->setSpacing(12);
	auto *scroll = new QScrollArea(this);
	scroll->setObjectName("sceneRoleScroll");
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *content = new QWidget(scroll);
	content->setObjectName("sceneRoleContent");
	scroll->setWidget(content);
	rootLayout->addWidget(scroll, 1);
	auto *mainLayout = new QVBoxLayout(content);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(12);

	// Header
	auto *lblHeader = new QLabel(this);
	lblHeader->setText(QString("تحديد وظيفة المشهد داخل بيئة [ %1 ]").arg(workspaceName));
	lblHeader->setObjectName("sceneRoleHeading");
	lblHeader->setWordWrap(true);
	mainLayout->addWidget(lblHeader);

	auto *lblSub = new QLabel(this);
	lblSub->setText(QString("المشهد المستهدف: <b>%1</b>").arg(sceneName));
	lblSub->setProperty("class", "scene-role-hint");
	lblSub->setWordWrap(true);
	mainLayout->addWidget(lblSub);

	auto *btnGroup = new QButtonGroup(this);

	// 1. Normal Scene Card
	auto *cardNormal = new QFrame(this);
	cardNormal->setProperty("class", "scene-role-card");
	auto *layNormal = new QVBoxLayout(cardNormal);
	layNormal->setContentsMargins(6, 6, 6, 6);
	layNormal->setSpacing(2);

	rbNormal_ = new SceneChoiceRadio("🎬  مشهد عادي (Normal Scene)", cardNormal);
	rbNormal_->setObjectName("sceneRoleNormal");
	rbNormal_->setCursor(Qt::PointingHandCursor);
	auto *descNormal = new QLabel("مشهد اعتيادي لا يشترك في التبديل التلقائي الذكي.", cardNormal);
	descNormal->setProperty("class", "scene-role-hint");
	descNormal->setWordWrap(true);
	layNormal->addWidget(rbNormal_);
	layNormal->addWidget(descNormal);
	btnGroup->addButton(rbNormal_);
	mainLayout->addWidget(cardNormal);

	// 2. Base Scene Card
	auto *cardBase = new QFrame(this);
	cardBase->setProperty("class", "scene-role-card");
	auto *layBase = new QVBoxLayout(cardBase);
	layBase->setContentsMargins(6, 6, 6, 6);
	layBase->setSpacing(2);

	rbBase_ = new SceneChoiceRadio("🏠  المشهد الأساسي (Base Scene)", cardBase);
	rbBase_->setObjectName("sceneRoleBase");
	rbBase_->setCursor(Qt::PointingHandCursor);
	auto *descBase = new QLabel("مشهد رئيسي ثابت لا يشارك في التبديل التلقائي.", cardBase);
	descBase->setProperty("class", "scene-role-hint");
	descBase->setWordWrap(true);
	layBase->addWidget(rbBase_);
	layBase->addWidget(descBase);
	btnGroup->addButton(rbBase_);
	mainLayout->addWidget(cardBase);

	// 3. Interactive Scene Card
	auto *cardInteractive = new QFrame(this);
	cardInteractive->setProperty("class", "scene-role-card");
	auto *layInteractive = new QVBoxLayout(cardInteractive);
	layInteractive->setContentsMargins(6, 6, 6, 6);
	layInteractive->setSpacing(4);

	rbInteractive_ = new SceneChoiceRadio("⚡  مشهد تفاعلي (Interactive Scene)", cardInteractive);
	rbInteractive_->setObjectName("sceneRoleInteractive");
	rbInteractive_->setCursor(Qt::PointingHandCursor);
	auto *descInteractive = new QLabel("عند نشاط المصدر يظهر هذا المشهد، وعند خمول المصدر ينتقل البرنامج إلى المشهد البديل.", cardInteractive);
	descInteractive->setProperty("class", "scene-role-hint");
	descInteractive->setWordWrap(true);
	layInteractive->addWidget(rbInteractive_);
	layInteractive->addWidget(descInteractive);
	btnGroup->addButton(rbInteractive_);

	// Subgroup for source selection inside Interactive
	sourceGroupWidget_ = new QWidget(cardInteractive);
	auto *laySourceGroup = new QVBoxLayout(sourceGroupWidget_);
	laySourceGroup->setContentsMargins(28, 6, 6, 4);
	laySourceGroup->setSpacing(4);

	auto *lblSourcePrompt = new QLabel("المصدر التفاعلي من المتغيرة في بيئة العمل:", sourceGroupWidget_);
	lblSourcePrompt->setProperty("class", "scene-role-label");
	laySourceGroup->addWidget(lblSourcePrompt);

	comboSources_ = new QComboBox(sourceGroupWidget_);
	comboSources_->addItem(QString::fromUtf8("-- اختر مصدرًا من المتغيرة --"), "");
	for (const auto &src : availableSources) {
		comboSources_->addItem("🎯  " + src, src);
	}

	if (!currentSource.isEmpty()) {
		int idx = comboSources_->findData(currentSource);
		if (idx >= 0) {
			comboSources_->setCurrentIndex(idx);
		}
	}
	laySourceGroup->addWidget(comboSources_);

	auto *lblFallbackPrompt = new QLabel("المشهد البديل عند خمول المصدر:", sourceGroupWidget_);
	lblFallbackPrompt->setProperty("class", "scene-role-label");
	laySourceGroup->addWidget(lblFallbackPrompt);

	comboFallbackScenes_ = new QComboBox(sourceGroupWidget_);
	comboFallbackScenes_->addItem("-- اختر المشهد البديل --", "");
	for (const auto &scene : availableFallbackScenes)
		comboFallbackScenes_->addItem("🎬  " + scene, scene);
	if (!currentFallbackScene.isEmpty()) {
		const int index = comboFallbackScenes_->findData(currentFallbackScene);
		if (index >= 0)
			comboFallbackScenes_->setCurrentIndex(index);
	}
	laySourceGroup->addWidget(comboFallbackScenes_);

	lblValidation_ = new QLabel(sourceGroupWidget_);
	lblValidation_->setObjectName("sceneRoleValidation");
	lblValidation_->setWordWrap(true);
	lblValidation_->setVisible(false);
	laySourceGroup->addWidget(lblValidation_);

	layInteractive->addWidget(sourceGroupWidget_);
	mainLayout->addWidget(cardInteractive);

	// Set initial selection
	if (currentRole == Role::Base) {
		rbBase_->setChecked(true);
		rbInteractive_->setEnabled(false);
		descInteractive->setText("المشهد الأساسي ثابت. غيّر دوره إلى عادي أولاً إذا أردت جعله تفاعليًا.");
	} else if (currentRole == Role::Interactive) {
		rbInteractive_->setChecked(true);
	} else {
		rbNormal_->setChecked(true);
	}

	// Connections
	auto onRoleToggled = [this]() {
		UpdateUIState();
	};
	connect(rbNormal_, &QRadioButton::toggled, this, onRoleToggled);
	connect(rbBase_, &QRadioButton::toggled, this, onRoleToggled);
	connect(rbInteractive_, &QRadioButton::toggled, this, onRoleToggled);

	connect(comboSources_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this]() { lblValidation_->setVisible(false); });
	connect(comboFallbackScenes_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this]() { lblValidation_->setVisible(false); });

	// 4. Text Elements Colors Card
	auto *cardTextColors = new QFrame(this);
	cardTextColors->setProperty("class", "scene-role-card");
	auto *layTextColors = new QVBoxLayout(cardTextColors);
	layTextColors->setContentsMargins(10, 8, 10, 8);
	layTextColors->setSpacing(8);

	auto *lblTextColorsTitle = new QLabel("🎨  ألوان العناصر الكتابية في هذا المشهد (العنوان، تاريخ، تنبيه الوقت، عداد الوقت)", cardTextColors);
	lblTextColorsTitle->setObjectName("sceneRoleColorsHeading");
	lblTextColorsTitle->setWordWrap(true);
	layTextColors->addWidget(lblTextColorsTitle);

	auto *colorModeGroup = new QButtonGroup(this);

	// Mode 1: Original / Default
	rbColorOriginal_ = new SceneChoiceRadio("🏠  اللون الأساسي الأصلي لكافة العناصر (افتراضي)", cardTextColors);
	rbColorOriginal_->setObjectName("sceneColorOriginal");
	rbColorOriginal_->setCursor(Qt::PointingHandCursor);
	rbColorOriginal_->setProperty("class", "scene-role-label");
	colorModeGroup->addButton(rbColorOriginal_);
	layTextColors->addWidget(rbColorOriginal_);

	// Mode 2: Unified Color
	rbColorUnified_ = new SceneChoiceRadio("🎨  لون موحّد لكافة العناصر الكتابية (تطبيق لون واحد عليها معاً)", cardTextColors);
	rbColorUnified_->setObjectName("sceneColorUnified");
	rbColorUnified_->setCursor(Qt::PointingHandCursor);
	rbColorUnified_->setProperty("class", "scene-role-label");
	colorModeGroup->addButton(rbColorUnified_);
	layTextColors->addWidget(rbColorUnified_);

	widgetUnifiedColor_ = new QWidget(cardTextColors);
	auto *layUnified = new QHBoxLayout(widgetUnifiedColor_);
	layUnified->setContentsMargins(28, 2, 6, 4);
	layUnified->setSpacing(10);

	auto *lblUnifiedPrompt = new QLabel("اللون الموحّد المختار لجميع العناصر:", widgetUnifiedColor_);
	lblUnifiedPrompt->setProperty("class", "scene-role-hint");
	layUnified->addWidget(lblUnifiedPrompt);

	btnUnifiedColor_ = new SceneColorButton(widgetUnifiedColor_);
	btnUnifiedColor_->setProperty("class", "scene-color-swatch");
	btnUnifiedColor_->setMinimumSize(140, 32);
	btnUnifiedColor_->setCursor(Qt::PointingHandCursor);
	layUnified->addWidget(btnUnifiedColor_);
	layUnified->addStretch(1);
	layTextColors->addWidget(widgetUnifiedColor_);

	connect(btnUnifiedColor_, &QPushButton::clicked, this, [this]() {
		QColor current(unifiedColorHex_);
		QColor chosen = QColorDialog::getColor(current, this, "اختر اللون الموحّد لكافة العناصر الكتابية");
		if (chosen.isValid()) {
			unifiedColorHex_ = chosen.name();
			UpdateUnifiedColorButton();
		}
	});

	// Mode 3: Custom per element
	rbColorCustom_ = new SceneChoiceRadio("🎛️  تحديد لون مخصص لكل عنصر على حدة (تخصيص فردي)", cardTextColors);
	rbColorCustom_->setObjectName("sceneColorCustom");
	rbColorCustom_->setCursor(Qt::PointingHandCursor);
	rbColorCustom_->setProperty("class", "scene-role-label");
	colorModeGroup->addButton(rbColorCustom_);
	layTextColors->addWidget(rbColorCustom_);

	widgetCustomColors_ = new QWidget(cardTextColors);
	auto *layCustom = new QVBoxLayout(widgetCustomColors_);
	layCustom->setContentsMargins(28, 2, 6, 4);
	layCustom->setSpacing(6);

	textElementRows_ = {
		{"العنوان", "🏷️  العنوان", nullptr, nullptr, nullptr, "#ffffff", "#ffffff"},
		{"تاريخ", "📅  تاريخ", nullptr, nullptr, nullptr, "#e2e8f0", "#e2e8f0"},
		{"تنبيه الوقت", "⏰  تنبيه الوقت", nullptr, nullptr, nullptr, "#f6ad55", "#f6ad55"},
		{"عداد الوقت", "⏱️  عداد الوقت", nullptr, nullptr, nullptr, "#63b3ed", "#63b3ed"}
	};

	for (size_t i = 0; i < textElementRows_.size(); ++i) {
		auto &row = textElementRows_[i];
		auto *rowWidget = new QWidget(widgetCustomColors_);
		auto *rowLayout = new QHBoxLayout(rowWidget);
		rowLayout->setContentsMargins(2, 2, 2, 2);
		rowLayout->setSpacing(8);

		row.chkEnable = new QCheckBox(row.displayName, rowWidget);
		row.chkEnable->setProperty("class", "scene-role-label");
		rowLayout->addWidget(row.chkEnable, 1);

		row.btnColor = new SceneColorButton(rowWidget);
		row.btnColor->setProperty("class", "scene-color-swatch");
		row.btnColor->setMinimumSize(112, 32);
		row.btnColor->setCursor(Qt::PointingHandCursor);
		rowLayout->addWidget(row.btnColor);

		row.btnReset = new QPushButton("افتراضي", rowWidget);
		row.btnReset->setMinimumSize(70, 32);
		row.btnReset->setCursor(Qt::PointingHandCursor);
		row.btnReset->setProperty("class", "scene-role-label");
		rowLayout->addWidget(row.btnReset);

		layCustom->addWidget(rowWidget);

		UpdateColorSwatch(i);

		size_t idx = i;
		connect(row.chkEnable, &QCheckBox::toggled, this, [this, idx](bool) {
			UpdateColorSwatch(idx);
		});

		connect(row.btnColor, &QPushButton::clicked, this, [this, idx]() {
			QColor current(textElementRows_[idx].currentColorHex);
			QColor chosen = QColorDialog::getColor(current, this, "اختر لون " + textElementRows_[idx].displayName);
			if (chosen.isValid()) {
				textElementRows_[idx].currentColorHex = chosen.name();
				textElementRows_[idx].chkEnable->setChecked(true);
				UpdateColorSwatch(idx);
			}
		});

		connect(row.btnReset, &QPushButton::clicked, this, [this, idx]() {
			textElementRows_[idx].currentColorHex = textElementRows_[idx].defaultColorHex;
			textElementRows_[idx].chkEnable->setChecked(false);
			UpdateColorSwatch(idx);
		});
	}
	layTextColors->addWidget(widgetCustomColors_);

	rbColorOriginal_->setChecked(true);
	UpdateUnifiedColorButton();
	UpdateColorModeUI();

	connect(rbColorOriginal_, &QRadioButton::toggled, this, [this](bool) { UpdateColorModeUI(); });
	connect(rbColorUnified_, &QRadioButton::toggled, this, [this](bool) { UpdateColorModeUI(); });
	connect(rbColorCustom_, &QRadioButton::toggled, this, [this](bool) { UpdateColorModeUI(); });

	mainLayout->addWidget(cardTextColors);

	mainLayout->addStretch(1);

	// Buttons
	auto *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(10);
	btnLayout->addStretch(1);

	btnCancel_ = new QPushButton("إلغاء", this);
	btnCancel_->setObjectName("btnCancel");
	btnCancel_->setCursor(Qt::PointingHandCursor);
	connect(btnCancel_, &QPushButton::clicked, this, &QDialog::reject);
	btnLayout->addWidget(btnCancel_);

	btnSave_ = new QPushButton("حفظ وتطبيق", this);
	btnSave_->setObjectName("btnSave");
	btnSave_->setCursor(Qt::PointingHandCursor);
	connect(btnSave_, &QPushButton::clicked, this, [this]() {
		if (GetSelectedRole() == Role::Interactive) {
			if (GetSelectedInteractiveSource().isEmpty()) {
				lblValidation_->setText("اختر مصدر التفاعل من المتغيرة في بيئة العمل الحالية.");
				lblValidation_->setVisible(true);
				return;
			}
			if (GetSelectedInteractiveFallbackScene().isEmpty()) {
				lblValidation_->setText("اختر المشهد البديل الذي يظهر عند خمول المصدر.");
				lblValidation_->setVisible(true);
				return;
			}
		}
		accept();
	});
	btnLayout->addWidget(btnSave_);

	rootLayout->addLayout(btnLayout);

	UpdateUIState();
}

void SceneRoleDialog::UpdateColorModeUI()
{
	if (widgetUnifiedColor_) {
		widgetUnifiedColor_->setVisible(rbColorUnified_ && rbColorUnified_->isChecked());
	}
	if (widgetCustomColors_) {
		widgetCustomColors_->setVisible(rbColorCustom_ && rbColorCustom_->isChecked());
	}
}

void SceneRoleDialog::UpdateUnifiedColorButton()
{
	if (!btnUnifiedColor_) return;
	QString hex = unifiedColorHex_.isEmpty() ? "#FFFFFF" : unifiedColorHex_;
	btnUnifiedColor_->setText("🎨  " + hex.toUpper());
	btnUnifiedColor_->setProperty("swatchColor", QColor(hex));
	btnUnifiedColor_->update();
}

void SceneRoleDialog::UpdateUIState()
{
	bool isInteractive = rbInteractive_->isChecked();
	sourceGroupWidget_->setEnabled(isInteractive);
	if (!isInteractive) {
		lblValidation_->setVisible(false);
	}
}

void SceneRoleDialog::UpdateColorSwatch(size_t index)
{
	if (index >= textElementRows_.size()) return;
	auto &row = textElementRows_[index];
	bool customActive = row.chkEnable->isChecked();
	QString hex = customActive ? row.currentColorHex : row.defaultColorHex;
	row.btnColor->setText(hex.toUpper());
	row.btnColor->setProperty("swatchColor", QColor(hex));
	row.btnColor->update();
}

void SceneRoleDialog::SetInitialTextColors(const std::map<std::string, std::string> &colors)
{
	if (colors.empty()) {
		rbColorOriginal_->setChecked(true);
		for (size_t i = 0; i < textElementRows_.size(); ++i) {
			textElementRows_[i].chkEnable->setChecked(false);
			UpdateColorSwatch(i);
		}
		UpdateColorModeUI();
		return;
	}

	// Check if all are identical (unified)
	bool allSame = true;
	std::string firstCol;
	size_t foundCount = 0;
	for (const auto &row : textElementRows_) {
		auto it = colors.find(row.id);
		if (it != colors.end() && !it->second.empty()) {
			foundCount++;
			if (firstCol.empty()) {
				firstCol = it->second;
			} else if (firstCol != it->second) {
				allSame = false;
			}
		}
	}

	if (allSame && foundCount >= 2 && !firstCol.empty()) {
		rbColorUnified_->setChecked(true);
		unifiedColorHex_ = QString::fromStdString(firstCol);
		UpdateUnifiedColorButton();
	} else {
		rbColorCustom_->setChecked(true);
		for (size_t i = 0; i < textElementRows_.size(); ++i) {
			auto &row = textElementRows_[i];
			auto it = colors.find(row.id);
			if (it != colors.end() && !it->second.empty()) {
				row.currentColorHex = QString::fromStdString(it->second);
				row.chkEnable->setChecked(true);
			} else {
				row.chkEnable->setChecked(false);
			}
			UpdateColorSwatch(i);
		}
	}

	UpdateColorModeUI();
}

std::map<std::string, std::string> SceneRoleDialog::GetCustomTextColors() const
{
	std::map<std::string, std::string> res;

	if (rbColorOriginal_ && rbColorOriginal_->isChecked()) {
		// Return empty map to clear and restore all original colors
		return res;
	}

	if (rbColorUnified_ && rbColorUnified_->isChecked()) {
		// Apply unified color to all 4 text elements
		std::string hex = unifiedColorHex_.toStdString();
		if (!hex.empty()) {
			res["العنوان"] = hex;
			res["تاريخ"] = hex;
			res["تنبيه الوقت"] = hex;
			res["عداد الوقت"] = hex;
		}
		return res;
	}

	// Custom per element
	for (const auto &row : textElementRows_) {
		if (row.chkEnable && row.chkEnable->isChecked()) {
			res[row.id] = row.currentColorHex.toStdString();
		}
	}
	return res;
}

SceneRoleDialog::Role SceneRoleDialog::GetSelectedRole() const
{
	if (rbBase_->isChecked()) return Role::Base;
	if (rbInteractive_->isChecked()) return Role::Interactive;
	return Role::Normal;
}

QString SceneRoleDialog::GetSelectedInteractiveSource() const
{
	if (!rbInteractive_->isChecked() || !comboSources_) return "";
	return comboSources_->currentData().toString();
}

QString SceneRoleDialog::GetSelectedInteractiveFallbackScene() const
{
	if (!rbInteractive_->isChecked() || !comboFallbackScenes_) return "";
	return comboFallbackScenes_->currentData().toString();
}

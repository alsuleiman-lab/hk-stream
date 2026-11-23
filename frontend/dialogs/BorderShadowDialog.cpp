#include "BorderShadowDialog.hpp"
#include <widgets/OBSBasic.hpp>
#include <workspaces/WorkspaceModel.hpp>
#include <qt-wrappers.hpp>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QSlider>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QColorDialog>
#include <QFrame>

#define BORDER_FILTER_ID "hk_border_filter"
#define BORDER_FILTER_NAME "حدود HK"

BorderShadowDialog::BorderShadowDialog(QWidget *parent, obs_source_t *source)
	: QDialog(parent), source_(source)
{
	aspectKey_ = OBSWorkspaceManager::GetActiveAspectRatioKey();
	setWindowTitle(QString::fromUtf8("🎨 إعدادات الحدود والظلال الاحترافية (%1)").arg(QString::fromStdString(aspectKey_)));
	setLayoutDirection(Qt::RightToLeft);
	setAttribute(Qt::WA_DeleteOnClose, false);
	setMinimumWidth(560);
	resize(600, 680);

	if (source_) {
		filter_ = obs_source_get_filter_by_name(source_, BORDER_FILTER_NAME);
		filterExistedBeforeDialog_ = filter_ != nullptr;
		initialFilterEnabled_ = filter_ && obs_source_enabled(filter_);
		if (filter_)
			initialSettings_ = obs_source_get_settings(filter_);

		obs_data_t *priv = obs_source_get_private_settings(source_);
		if (priv) {
			std::string key = "hk_border_" + aspectKey_;
			obs_data_t *aspectData = obs_data_get_obj(priv, key.c_str());
			if (aspectData) {
				if (!initialSettings_) {
					initialSettings_ = obs_data_create();
					obs_data_apply(initialSettings_, aspectData);
				}
				obs_data_release(aspectData);
			}
			obs_data_release(priv);
		}
	}

	buildUI();
	applyStylesheet();
	loadSettings();
}

void BorderShadowDialog::buildUI()
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(20, 18, 20, 18);
	mainLayout->setSpacing(14);

	// Top banner with source name
	auto *headerCard = new QFrame(this);
	headerCard->setObjectName("borderHeaderCard");
	auto *headerLayout = new QHBoxLayout(headerCard);
	headerLayout->setContentsMargins(14, 10, 14, 10);

	const char *srcName = source_ ? obs_source_get_name(source_) : "";
	auto *lblTitle = new QLabel(QString::fromUtf8("المصدر المستهدف: <b>%1</b> &nbsp; | &nbsp; مقاس العرض: <span style='color: var(--primary); font-weight: bold;'>%2</span>")
		.arg(QT_UTF8(srcName))
		.arg(QString::fromStdString(aspectKey_)), headerCard);
	lblTitle->setObjectName("lblTargetSource");
	headerLayout->addWidget(lblTitle);
	mainLayout->addWidget(headerCard);

	// Quick Presets
	auto *presetCard = new QFrame(this);
	presetCard->setObjectName("borderPresetCard");
	auto *presetLayout = new QHBoxLayout(presetCard);
	presetLayout->setContentsMargins(8, 6, 8, 6);
	presetLayout->setSpacing(8);

	auto *lblPreset = new QLabel(QString::fromUtf8("قوالب سريعة:"), presetCard);
	lblPreset->setStyleSheet("font-weight: bold;");
	presetLayout->addWidget(lblPreset);

	auto makePresetBtn = [this, presetLayout, presetCard](const QString &text, int id) {
		auto *btn = new QPushButton(text, presetCard);
		btn->setObjectName("btnPresetItem");
		btn->setCursor(Qt::PointingHandCursor);
		connect(btn, &QPushButton::clicked, this, [this, id]() { onApplyPreset(id); });
		presetLayout->addWidget(btn);
	};

	makePresetBtn(QString::fromUtf8("🥇 ذهبي ملكي"), 1);
	makePresetBtn(QString::fromUtf8("💎 كشمير ناعم"), 2);
	makePresetBtn(QString::fromUtf8("🌿 زمردي أنيق"), 3);
	makePresetBtn(QString::fromUtf8("☁️ ظل عائم ثلاثي"), 4);
	mainLayout->addWidget(presetCard);

	// 1. BORDER GROUP
	auto *groupBorder = new QGroupBox(QString::fromUtf8("🛡️ الحد الخارجي (Border)"), this);
	groupBorder->setObjectName("groupBorder");
	auto *borderGrid = new QGridLayout(groupBorder);
	borderGrid->setContentsMargins(14, 16, 14, 14);
	borderGrid->setHorizontalSpacing(12);
	borderGrid->setVerticalSpacing(10);

	chkBorderEnable_ = new QCheckBox(QString::fromUtf8("تفعيل الحد"), groupBorder);
	chkBorderEnable_->setCursor(Qt::PointingHandCursor);
	connect(chkBorderEnable_, &QCheckBox::toggled, this, &BorderShadowDialog::onBorderToggled);
	borderGrid->addWidget(chkBorderEnable_, 0, 0, 1, 3);

	// Border Color
	borderGrid->addWidget(new QLabel(QString::fromUtf8("لون الحد:"), groupBorder), 1, 0);
	btnBorderColor_ = new QPushButton(groupBorder);
	btnBorderColor_->setObjectName("btnColorPicker");
	btnBorderColor_->setFixedHeight(30);
	btnBorderColor_->setCursor(Qt::PointingHandCursor);
	connect(btnBorderColor_, &QPushButton::clicked, this, &BorderShadowDialog::onBorderColorClicked);
	borderGrid->addWidget(btnBorderColor_, 1, 1, 1, 2);

	// Border Thickness
	borderGrid->addWidget(new QLabel(QString::fromUtf8("سُمك الحد:"), groupBorder), 2, 0);
	sliderThickness_ = new QSlider(Qt::Horizontal, groupBorder);
	sliderThickness_->setRange(1, 100);
	spinThickness_ = new QSpinBox(groupBorder);
	spinThickness_->setRange(1, 100);
	spinThickness_->setSuffix(QString::fromUtf8(" px"));
	spinThickness_->setFixedWidth(80);
	connect(sliderThickness_, &QSlider::valueChanged, spinThickness_, &QSpinBox::setValue);
	connect(spinThickness_, QOverload<int>::of(&QSpinBox::valueChanged), sliderThickness_, &QSlider::setValue);
	connect(sliderThickness_, &QSlider::valueChanged, this, &BorderShadowDialog::onBorderThicknessChanged);
	borderGrid->addWidget(sliderThickness_, 2, 1);
	borderGrid->addWidget(spinThickness_, 2, 2);

	// Round Corners
	chkRoundCorners_ = new QCheckBox(QString::fromUtf8("زوايا منحنية (دائرية)"), groupBorder);
	chkRoundCorners_->setCursor(Qt::PointingHandCursor);
	connect(chkRoundCorners_, &QCheckBox::toggled, this, &BorderShadowDialog::onRoundCornersToggled);
	borderGrid->addWidget(chkRoundCorners_, 3, 0, 1, 3);

	// Corner Radius
	borderGrid->addWidget(new QLabel(QString::fromUtf8("نصف قطر الاستدارة:"), groupBorder), 4, 0);
	sliderRadius_ = new QSlider(Qt::Horizontal, groupBorder);
	sliderRadius_->setRange(0, 300);
	spinRadius_ = new QSpinBox(groupBorder);
	spinRadius_->setRange(0, 300);
	spinRadius_->setSuffix(QString::fromUtf8(" px"));
	spinRadius_->setFixedWidth(80);
	connect(sliderRadius_, &QSlider::valueChanged, spinRadius_, &QSpinBox::setValue);
	connect(spinRadius_, QOverload<int>::of(&QSpinBox::valueChanged), sliderRadius_, &QSlider::setValue);
	connect(sliderRadius_, &QSlider::valueChanged, this, &BorderShadowDialog::onCornerRadiusChanged);
	borderGrid->addWidget(sliderRadius_, 4, 1);
	borderGrid->addWidget(spinRadius_, 4, 2);

	mainLayout->addWidget(groupBorder);

	// 2. SHADOW GROUP
	auto *groupShadow = new QGroupBox(QString::fromUtf8("✨ الظل (Drop Shadow)"), this);
	groupShadow->setObjectName("groupShadow");
	auto *shadowGrid = new QGridLayout(groupShadow);
	shadowGrid->setContentsMargins(14, 16, 14, 14);
	shadowGrid->setHorizontalSpacing(12);
	shadowGrid->setVerticalSpacing(10);

	chkShadowEnable_ = new QCheckBox(QString::fromUtf8("تفعيل الظل"), groupShadow);
	chkShadowEnable_->setCursor(Qt::PointingHandCursor);
	connect(chkShadowEnable_, &QCheckBox::toggled, this, &BorderShadowDialog::onShadowToggled);
	shadowGrid->addWidget(chkShadowEnable_, 0, 0, 1, 3);

	// Shadow Color
	shadowGrid->addWidget(new QLabel(QString::fromUtf8("لون الظل:"), groupShadow), 1, 0);
	btnShadowColor_ = new QPushButton(groupShadow);
	btnShadowColor_->setObjectName("btnColorPicker");
	btnShadowColor_->setFixedHeight(30);
	btnShadowColor_->setCursor(Qt::PointingHandCursor);
	connect(btnShadowColor_, &QPushButton::clicked, this, &BorderShadowDialog::onShadowColorClicked);
	shadowGrid->addWidget(btnShadowColor_, 1, 1, 1, 2);

	// Shadow Opacity
	shadowGrid->addWidget(new QLabel(QString::fromUtf8("شفافية الظل:"), groupShadow), 2, 0);
	sliderShadowOpacity_ = new QSlider(Qt::Horizontal, groupShadow);
	sliderShadowOpacity_->setRange(0, 100);
	spinShadowOpacity_ = new QSpinBox(groupShadow);
	spinShadowOpacity_->setRange(0, 100);
	spinShadowOpacity_->setSuffix(QString::fromUtf8(" %"));
	spinShadowOpacity_->setFixedWidth(80);
	connect(sliderShadowOpacity_, &QSlider::valueChanged, spinShadowOpacity_, &QSpinBox::setValue);
	connect(spinShadowOpacity_, QOverload<int>::of(&QSpinBox::valueChanged), sliderShadowOpacity_, &QSlider::setValue);
	connect(sliderShadowOpacity_, &QSlider::valueChanged, this, &BorderShadowDialog::onShadowOpacityChanged);
	shadowGrid->addWidget(sliderShadowOpacity_, 2, 1);
	shadowGrid->addWidget(spinShadowOpacity_, 2, 2);

	// Shadow Blur
	shadowGrid->addWidget(new QLabel(QString::fromUtf8("نعومة وضبابية الظل:"), groupShadow), 3, 0);
	sliderShadowBlur_ = new QSlider(Qt::Horizontal, groupShadow);
	sliderShadowBlur_->setRange(0, 100);
	spinShadowBlur_ = new QSpinBox(groupShadow);
	spinShadowBlur_->setRange(0, 100);
	spinShadowBlur_->setSuffix(QString::fromUtf8(" px"));
	spinShadowBlur_->setFixedWidth(80);
	connect(sliderShadowBlur_, &QSlider::valueChanged, spinShadowBlur_, &QSpinBox::setValue);
	connect(spinShadowBlur_, QOverload<int>::of(&QSpinBox::valueChanged), sliderShadowBlur_, &QSlider::setValue);
	connect(sliderShadowBlur_, &QSlider::valueChanged, this, &BorderShadowDialog::onShadowBlurChanged);
	shadowGrid->addWidget(sliderShadowBlur_, 3, 1);
	shadowGrid->addWidget(spinShadowBlur_, 3, 2);

	// Offset X
	shadowGrid->addWidget(new QLabel(QString::fromUtf8("إزاحة أفقية (X):"), groupShadow), 4, 0);
	sliderOffsetX_ = new QSlider(Qt::Horizontal, groupShadow);
	sliderOffsetX_->setRange(-100, 100);
	spinOffsetX_ = new QSpinBox(groupShadow);
	spinOffsetX_->setRange(-100, 100);
	spinOffsetX_->setSuffix(QString::fromUtf8(" px"));
	spinOffsetX_->setFixedWidth(80);
	connect(sliderOffsetX_, &QSlider::valueChanged, spinOffsetX_, &QSpinBox::setValue);
	connect(spinOffsetX_, QOverload<int>::of(&QSpinBox::valueChanged), sliderOffsetX_, &QSlider::setValue);
	connect(sliderOffsetX_, &QSlider::valueChanged, this, &BorderShadowDialog::onShadowOffsetXChanged);
	shadowGrid->addWidget(sliderOffsetX_, 4, 1);
	shadowGrid->addWidget(spinOffsetX_, 4, 2);

	// Offset Y
	shadowGrid->addWidget(new QLabel(QString::fromUtf8("إزاحة عمودية (Y):"), groupShadow), 5, 0);
	sliderOffsetY_ = new QSlider(Qt::Horizontal, groupShadow);
	sliderOffsetY_->setRange(-100, 100);
	spinOffsetY_ = new QSpinBox(groupShadow);
	spinOffsetY_->setRange(-100, 100);
	spinOffsetY_->setSuffix(QString::fromUtf8(" px"));
	spinOffsetY_->setFixedWidth(80);
	connect(sliderOffsetY_, &QSlider::valueChanged, spinOffsetY_, &QSpinBox::setValue);
	connect(spinOffsetY_, QOverload<int>::of(&QSpinBox::valueChanged), sliderOffsetY_, &QSlider::setValue);
	connect(sliderOffsetY_, &QSlider::valueChanged, this, &BorderShadowDialog::onShadowOffsetYChanged);
	shadowGrid->addWidget(sliderOffsetY_, 5, 1);
	shadowGrid->addWidget(spinOffsetY_, 5, 2);

	mainLayout->addWidget(groupShadow);

	// Bottom action buttons
	auto *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(10);

	auto *btnSave = new QPushButton(QString::fromUtf8("✓ حفظ وتطبيق"), this);
	btnSave->setObjectName("btnSaveBorder");
	btnSave->setFixedHeight(36);
	btnSave->setCursor(Qt::PointingHandCursor);
	connect(btnSave, &QPushButton::clicked, this, &BorderShadowDialog::onSaveClicked);
	btnLayout->addWidget(btnSave, 2);

	auto *btnReset = new QPushButton(QString::fromUtf8("🗑️ إزالة الحدود والظل"), this);
	btnReset->setObjectName("btnResetBorder");
	btnReset->setFixedHeight(36);
	btnReset->setCursor(Qt::PointingHandCursor);
	connect(btnReset, &QPushButton::clicked, this, &BorderShadowDialog::onResetClicked);
	btnLayout->addWidget(btnReset, 1);

	auto *btnCancel = new QPushButton(QString::fromUtf8("إلغاء"), this);
	btnCancel->setObjectName("btnCancelBorder");
	btnCancel->setFixedHeight(36);
	btnCancel->setCursor(Qt::PointingHandCursor);
	connect(btnCancel, &QPushButton::clicked, this, &BorderShadowDialog::onCancelClicked);
	btnLayout->addWidget(btnCancel, 1);

	mainLayout->addLayout(btnLayout);
}

void BorderShadowDialog::loadSettings()
{
	bool bEnable = false;
	uint32_t bColor = 0xFF4FA6E3;
	int thickness = 12;
	bool roundCorners = true;
	int radius = 24;

	bool sEnable = false;
	uint32_t sColor = 0x8C000000;
	int sBlur = 20;
	int sOffX = 0;
	int sOffY = 10;

	if (initialSettings_) {
		bEnable = obs_data_get_bool(initialSettings_, "border_enabled");
		bColor = (uint32_t)obs_data_get_int(initialSettings_, "color");
		thickness = (int)obs_data_get_int(initialSettings_, "thickness");
		roundCorners = obs_data_get_bool(initialSettings_, "round_corners");
		radius = (int)obs_data_get_int(initialSettings_, "corner_radius");

		sEnable = obs_data_get_bool(initialSettings_, "shadow_enabled");
		sColor = (uint32_t)obs_data_get_int(initialSettings_, "shadow_color");
		sBlur = (int)obs_data_get_int(initialSettings_, "shadow_blur");
		sOffX = (int)obs_data_get_int(initialSettings_, "shadow_offset_x");
		sOffY = (int)obs_data_get_int(initialSettings_, "shadow_offset_y");
	}

	chkBorderEnable_->setChecked(bEnable);
	sliderThickness_->setValue(thickness > 0 ? thickness : 12);
	chkRoundCorners_->setChecked(roundCorners);
	sliderRadius_->setValue(radius > 0 ? radius : 24);

	borderColor_ = QColor((bColor >> 16) & 0xFF, (bColor >> 8) & 0xFF, bColor & 0xFF, (bColor >> 24) & 0xFF);

	chkShadowEnable_->setChecked(sEnable);
	const int shadowA = (sColor >> 24) & 0xFF;
	shadowColor_ = QColor((sColor >> 16) & 0xFF, (sColor >> 8) & 0xFF, sColor & 0xFF, shadowA);
	sliderShadowOpacity_->setValue((int)qRound(shadowA * 100.0 / 255.0));
	sliderShadowBlur_->setValue(sBlur > 0 ? sBlur : 20);
	sliderOffsetX_->setValue(sOffX);
	sliderOffsetY_->setValue(sOffY > 0 ? sOffY : 10);

	updateColorPreviews();
}

void BorderShadowDialog::applyLiveSettings()
{
	if (!source_) return;

	if (!filter_) {
		OBSDataAutoRelease empty = obs_data_create();
		filter_ = obs_source_create_private(BORDER_FILTER_ID, BORDER_FILTER_NAME, empty);
		if (filter_) {
			obs_source_filter_add(source_, filter_);
			filterCreatedByUs_ = true;
		}
	}
	if (!filter_) return;

	OBSDataAutoRelease s = obs_data_create();

	obs_data_set_bool(s, "border_enabled", chkBorderEnable_->isChecked());
	const uint32_t bVal = ((uint32_t)borderColor_.alpha() << 24) |
			      ((uint32_t)borderColor_.red() << 16) |
			      ((uint32_t)borderColor_.green() << 8) |
			      ((uint32_t)borderColor_.blue());
	obs_data_set_int(s, "color", bVal);
	obs_data_set_int(s, "thickness", sliderThickness_->value());
	obs_data_set_bool(s, "round_corners", chkRoundCorners_->isChecked());
	obs_data_set_int(s, "corner_radius", sliderRadius_->value());

	obs_data_set_bool(s, "shadow_enabled", chkShadowEnable_->isChecked());
	const int shadowA = (int)qRound(sliderShadowOpacity_->value() * 255.0 / 100.0);
	const uint32_t sVal = ((uint32_t)shadowA << 24) |
			      ((uint32_t)shadowColor_.red() << 16) |
			      ((uint32_t)shadowColor_.green() << 8) |
			      ((uint32_t)shadowColor_.blue());
	obs_data_set_int(s, "shadow_color", sVal);
	obs_data_set_int(s, "shadow_blur", sliderShadowBlur_->value());
	obs_data_set_int(s, "shadow_offset_x", sliderOffsetX_->value());
	obs_data_set_int(s, "shadow_offset_y", sliderOffsetY_->value());

	obs_source_update(filter_, s);
	obs_source_set_enabled(filter_, chkBorderEnable_->isChecked() || chkShadowEnable_->isChecked());
}

void BorderShadowDialog::updateColorPreviews()
{
	btnBorderColor_->setText(QString::fromUtf8("  %1 (انقر للتغيير)").arg(borderColor_.name().toUpper()));
	btnBorderColor_->setStyleSheet(QString(
		"#btnColorPicker { background-color: %1; color: %2; font-weight: bold; border-radius: 4px; border: 1px solid var(--border); }")
		.arg(borderColor_.name())
		.arg(borderColor_.lightness() > 140 ? "#000000" : "#FFFFFF"));

	btnShadowColor_->setText(QString::fromUtf8("  %1 (انقر للتغيير)").arg(shadowColor_.name().toUpper()));
	btnShadowColor_->setStyleSheet(QString(
		"#btnColorPicker { background-color: %1; color: %2; font-weight: bold; border-radius: 4px; border: 1px solid var(--border); }")
		.arg(shadowColor_.name())
		.arg(shadowColor_.lightness() > 140 ? "#000000" : "#FFFFFF"));
}

void BorderShadowDialog::onBorderToggled(bool checked)
{
	btnBorderColor_->setEnabled(checked);
	sliderThickness_->setEnabled(checked);
	spinThickness_->setEnabled(checked);
	chkRoundCorners_->setEnabled(checked);
	sliderRadius_->setEnabled(checked && chkRoundCorners_->isChecked());
	spinRadius_->setEnabled(checked && chkRoundCorners_->isChecked());
	applyLiveSettings();
}

void BorderShadowDialog::onBorderColorClicked()
{
	QColor c = QColorDialog::getColor(borderColor_, this, QString::fromUtf8("اختر لون الحد"));
	if (c.isValid()) {
		borderColor_ = c;
		updateColorPreviews();
		applyLiveSettings();
	}
}

void BorderShadowDialog::onBorderThicknessChanged(int)
{
	applyLiveSettings();
}

void BorderShadowDialog::onRoundCornersToggled(bool checked)
{
	sliderRadius_->setEnabled(checked);
	spinRadius_->setEnabled(checked);
	applyLiveSettings();
}

void BorderShadowDialog::onCornerRadiusChanged(int)
{
	applyLiveSettings();
}

void BorderShadowDialog::onShadowToggled(bool checked)
{
	btnShadowColor_->setEnabled(checked);
	sliderShadowOpacity_->setEnabled(checked);
	spinShadowOpacity_->setEnabled(checked);
	sliderShadowBlur_->setEnabled(checked);
	spinShadowBlur_->setEnabled(checked);
	sliderOffsetX_->setEnabled(checked);
	spinOffsetX_->setEnabled(checked);
	sliderOffsetY_->setEnabled(checked);
	spinOffsetY_->setEnabled(checked);
	applyLiveSettings();
}

void BorderShadowDialog::onShadowColorClicked()
{
	QColor c = QColorDialog::getColor(shadowColor_, this, QString::fromUtf8("اختر لون الظل"));
	if (c.isValid()) {
		shadowColor_ = c;
		updateColorPreviews();
		applyLiveSettings();
	}
}

void BorderShadowDialog::onShadowOpacityChanged(int)
{
	applyLiveSettings();
}

void BorderShadowDialog::onShadowBlurChanged(int)
{
	applyLiveSettings();
}

void BorderShadowDialog::onShadowOffsetXChanged(int)
{
	applyLiveSettings();
}

void BorderShadowDialog::onShadowOffsetYChanged(int)
{
	applyLiveSettings();
}

void BorderShadowDialog::onApplyPreset(int presetId)
{
	switch (presetId) {
	case 1: // Royal Gold
		chkBorderEnable_->setChecked(true);
		borderColor_ = QColor(218, 165, 32); // Goldenrod
		sliderThickness_->setValue(14);
		chkRoundCorners_->setChecked(true);
		sliderRadius_->setValue(28);

		chkShadowEnable_->setChecked(true);
		shadowColor_ = QColor(20, 10, 5);
		sliderShadowOpacity_->setValue(65);
		sliderShadowBlur_->setValue(26);
		sliderOffsetX_->setValue(0);
		sliderOffsetY_->setValue(12);
		break;
	case 2: // Soft Cashmere
		chkBorderEnable_->setChecked(true);
		borderColor_ = QColor(210, 195, 180); // Cashmere
		sliderThickness_->setValue(10);
		chkRoundCorners_->setChecked(true);
		sliderRadius_->setValue(20);

		chkShadowEnable_->setChecked(true);
		shadowColor_ = QColor(10, 10, 10);
		sliderShadowOpacity_->setValue(45);
		sliderShadowBlur_->setValue(18);
		sliderOffsetX_->setValue(0);
		sliderOffsetY_->setValue(8);
		break;
	case 3: // Emerald Accent
		chkBorderEnable_->setChecked(true);
		borderColor_ = QColor(26, 120, 80); // Emerald
		sliderThickness_->setValue(12);
		chkRoundCorners_->setChecked(true);
		sliderRadius_->setValue(22);

		chkShadowEnable_->setChecked(true);
		shadowColor_ = QColor(0, 0, 0);
		sliderShadowOpacity_->setValue(55);
		sliderShadowBlur_->setValue(22);
		sliderOffsetX_->setValue(0);
		sliderOffsetY_->setValue(10);
		break;
	case 4: // Floating 3D Shadow
		chkBorderEnable_->setChecked(false);
		chkRoundCorners_->setChecked(true);
		sliderRadius_->setValue(24);

		chkShadowEnable_->setChecked(true);
		shadowColor_ = QColor(0, 0, 0);
		sliderShadowOpacity_->setValue(70);
		sliderShadowBlur_->setValue(35);
		sliderOffsetX_->setValue(0);
		sliderOffsetY_->setValue(18);
		break;
	}

	updateColorPreviews();
	applyLiveSettings();
}

void BorderShadowDialog::onResetClicked()
{
	if (source_) {
		obs_data_t *priv = obs_source_get_private_settings(source_);
		if (priv) {
			std::string key = "hk_border_" + aspectKey_;
			obs_data_erase(priv, key.c_str());
			obs_data_release(priv);
		}
	}

	if (filter_) {
		// Removing the per-item filter records the user's reset explicitly.
		// Leaving it disabled with border_enabled=true would make the scene
		// loader mistake it for a filter disabled by the old aspect updater.
		if (source_)
			obs_source_filter_remove(source_, filter_);
		filter_ = nullptr;
	}

	OBSBasic *main = OBSBasic::Get();
	if (main) main->SaveProject();
	accept();
}

void BorderShadowDialog::onSaveClicked()
{
	applyLiveSettings();

	if (source_) {
		obs_data_t *priv = obs_source_get_private_settings(source_);
		if (priv) {
			std::string key = "hk_border_" + aspectKey_;
			obs_data_erase(priv, key.c_str());
			obs_data_release(priv);
		}
	}

	OBSBasic *main = OBSBasic::Get();
	if (main) main->SaveProject();
	accept();
}

void BorderShadowDialog::onCancelClicked()
{
	if (filter_ && filterCreatedByUs_ && !filterExistedBeforeDialog_ && source_) {
		obs_source_filter_remove(source_, filter_);
		filter_ = nullptr;
	} else if (initialSettings_ && filter_) {
		obs_source_update(filter_, initialSettings_);
		obs_source_set_enabled(filter_, initialFilterEnabled_);
	} else if (filter_) {
		obs_source_set_enabled(filter_, initialFilterEnabled_);
	}
	reject();
}

void BorderShadowDialog::applyStylesheet()
{
	setStyleSheet(QString::fromUtf8(
		"QDialog {"
		"    background-color: var(--bg_window);"
		"    color: var(--text);"
		"    font-size: var(--font_base);"
		"}"
		"#borderHeaderCard {"
		"    background-color: var(--bg_base);"
		"    border: 1px solid var(--border);"
		"    border-radius: var(--border_radius);"
		"}"
		"#borderPresetCard {"
		"    background-color: var(--bg_base);"
		"    border: 1px solid var(--border);"
		"    border-radius: var(--border_radius);"
		"}"
		"#btnPresetItem {"
		"    background-color: var(--button_bg);"
		"    color: var(--text);"
		"    border: 1px solid var(--border);"
		"    border-radius: 4px;"
		"    padding: 4px 8px;"
		"    font-size: var(--font_small);"
		"}"
		"#btnPresetItem:hover {"
		"    background-color: var(--primary);"
		"    color: #FFFFFF;"
		"}"
		"QGroupBox {"
		"    background-color: var(--bg_base);"
		"    border: 1px solid var(--border);"
		"    border-radius: var(--border_radius);"
		"    margin-top: 18px;"
		"    font-weight: bold;"
		"    padding-top: 12px;"
		"}"
		"QGroupBox::title {"
		"    subcontrol-origin: margin;"
		"    subcontrol-position: top right;"
		"    padding: 0 8px;"
		"    color: var(--primary);"
		"}"
		"#btnSaveBorder {"
		"    background-color: var(--primary);"
		"    color: #FFFFFF;"
		"    font-weight: bold;"
		"    border-radius: var(--border_radius);"
		"    border: none;"
		"}"
		"#btnSaveBorder:hover {"
		"    background-color: var(--primary_hover);"
		"}"
		"#btnResetBorder {"
		"    background-color: var(--button_bg);"
		"    color: #E53E3E;"
		"    font-weight: bold;"
		"    border: 1px solid var(--border);"
		"    border-radius: var(--border_radius);"
		"}"
		"#btnResetBorder:hover {"
		"    background-color: #FEB2B2;"
		"    color: #742A2A;"
		"}"
		"#btnCancelBorder {"
		"    background-color: var(--button_bg);"
		"    color: var(--text);"
		"    border: 1px solid var(--border);"
		"    border-radius: var(--border_radius);"
		"}"
	));
}

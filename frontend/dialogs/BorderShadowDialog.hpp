#pragma once

#include <QDialog>
#include <obs.hpp>
#include <QColor>

class QCheckBox;
class QSlider;
class QSpinBox;
class QPushButton;
class QLabel;

class BorderShadowDialog : public QDialog {
	Q_OBJECT

public:
	explicit BorderShadowDialog(QWidget *parent, obs_source_t *source);
	~BorderShadowDialog() override = default;

private slots:
	void onBorderToggled(bool checked);
	void onBorderColorClicked();
	void onBorderThicknessChanged(int val);
	void onRoundCornersToggled(bool checked);
	void onCornerRadiusChanged(int val);

	void onShadowToggled(bool checked);
	void onShadowColorClicked();
	void onShadowOpacityChanged(int val);
	void onShadowBlurChanged(int val);
	void onShadowOffsetXChanged(int val);
	void onShadowOffsetYChanged(int val);

	void onApplyPreset(int presetId);
	void onResetClicked();
	void onSaveClicked();
	void onCancelClicked();

private:
	void buildUI();
	void applyStylesheet();
	void loadSettings();
	void applyLiveSettings();
	void updateColorPreviews();

	OBSSource source_;
	OBSSourceAutoRelease filter_;
	bool filterCreatedByUs_ = false;
	bool filterExistedBeforeDialog_ = false;
	bool initialFilterEnabled_ = false;
	OBSData initialSettings_ = nullptr;
	std::string aspectKey_;

	// UI Controls - Border
	QCheckBox *chkBorderEnable_ = nullptr;
	QPushButton *btnBorderColor_ = nullptr;
	QSlider *sliderThickness_ = nullptr;
	QSpinBox *spinThickness_ = nullptr;
	QCheckBox *chkRoundCorners_ = nullptr;
	QSlider *sliderRadius_ = nullptr;
	QSpinBox *spinRadius_ = nullptr;

	// UI Controls - Shadow
	QCheckBox *chkShadowEnable_ = nullptr;
	QPushButton *btnShadowColor_ = nullptr;
	QSlider *sliderShadowOpacity_ = nullptr;
	QSpinBox *spinShadowOpacity_ = nullptr;
	QSlider *sliderShadowBlur_ = nullptr;
	QSpinBox *spinShadowBlur_ = nullptr;
	QSlider *sliderOffsetX_ = nullptr;
	QSpinBox *spinOffsetX_ = nullptr;
	QSlider *sliderOffsetY_ = nullptr;
	QSpinBox *spinOffsetY_ = nullptr;

	QColor borderColor_ = QColor(79, 166, 227);
	QColor shadowColor_ = QColor(0, 0, 0, 140);
};

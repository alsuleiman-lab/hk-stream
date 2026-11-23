#include "OBSHeader.hpp"
#include "OBSBasic.hpp"
#include "utility/MultiStreamManager.hpp"
#include <QColor>
#include <QComboBox>
#include <QAbstractItemView>
#include <QEasingCurve>
#include <QPalette>
#include <QRadialGradient>
#include <QStyle>
#include <QSizePolicy>
#include <QStringList>
#include <QFile>
#include <QPainter>
#include <QPaintEvent>
#include <QSvgRenderer>
#include <QVariantAnimation>
#include <QLabel>
#include <QTimer>

namespace {
class HeaderIconButton final : public QPushButton {
public:
	HeaderIconButton(const QString &iconResource, QWidget *parent) : QPushButton(parent)
	{
		setText(QString());
		setFocusPolicy(Qt::StrongFocus);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		setVectorIcon(iconResource);
	}

	void setVectorIcon(const QString &iconResource)
	{
		QFile file(iconResource);
		if (file.open(QIODevice::ReadOnly))
			iconSvg_ = file.readAll();
		else
			iconSvg_.clear();
		update();
	}

	void setGlow(const QColor &color, qreal progress)
	{
		glowColor_ = color;
		glowProgress_ = qBound<qreal>(0.0, progress, 1.0);
		update();
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		QPushButton::paintEvent(event);

		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const bool active = property("active").toBool() || property("recording").toBool() ||
			property("paused").toBool();
		QColor ink;
		if (active) {
			ink = palette().color(QPalette::HighlightedText);
		} else if (objectName() == QLatin1String("btnManageTitles")) {
			ink = palette().color(QPalette::ButtonText);
		} else if (objectName() == QLatin1String("btnRecord")) {
			ink = palette().color(QPalette::BrightText);
		} else {
			ink = palette().color(QPalette::Link);
		}
		if (iconSvg_.isEmpty())
			return;

		if (glowColor_.isValid() && glowProgress_ > 0.0) {
			const QPointF center(width() / 2.0, height() / 2.0);
			const qreal radius = 13.0 + 7.0 * glowProgress_;
			const QColor hsv = glowColor_.toHsv();
			int hue = hsv.hue();
			if (hue < 0)
				hue = 150;
			QColor centerGlow;
			centerGlow.setHsv(hue, qMin(hsv.saturation() / 3, 76), 255, qRound(96.0 + 128.0 * glowProgress_));
			QColor transparentGlow = centerGlow;
			transparentGlow.setAlpha(0);
			QRadialGradient halo(center, radius);
			halo.setColorAt(0.0, transparentGlow);
			halo.setColorAt(0.42, transparentGlow);
			halo.setColorAt(0.7, centerGlow);
			halo.setColorAt(1.0, transparentGlow);
			painter.setPen(Qt::NoPen);
			painter.setBrush(halo);
			painter.drawEllipse(QRectF(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0));
		}

		QByteArray themedSvg = iconSvg_;
		themedSvg.replace("currentColor", ink.name(QColor::HexRgb).toUtf8());
		QSvgRenderer renderer(themedSvg);
		const qreal iconSize = 24.0;
		const QRectF iconRect((width() - iconSize) / 2.0, (height() - iconSize) / 2.0, iconSize, iconSize);
		renderer.render(&painter, iconRect);
	}

private:
	QByteArray iconSvg_;
	QColor glowColor_;
	qreal glowProgress_ = 0.0;
};

void RefreshButtonStyle(QPushButton *button)
{
	if (!button)
		return;
	button->style()->unpolish(button);
	button->style()->polish(button);
	button->update();
}

void SetHeaderButtonIcon(QPushButton *button, const QString &resource)
{
	if (auto *iconButton = dynamic_cast<HeaderIconButton *>(button))
		iconButton->setVectorIcon(resource);
}
}

// -------------------------------------------------------
// Constructor
// -------------------------------------------------------
OBSHeader::OBSHeader(OBSBasic *main, QWidget *parent)
	: QWidget(parent), main_(main)
{
	setObjectName("OBSHeader");
	setLayoutDirection(Qt::RightToLeft);
	setFixedHeight(46);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	buildUI();
	applyStylesheet();

	glowAnimation_ = new QVariantAnimation(this);
	glowAnimation_->setStartValue(0.0);
	glowAnimation_->setKeyValueAt(0.5, 1.0);
	glowAnimation_->setEndValue(0.0);
	glowAnimation_->setDuration(1800);
	glowAnimation_->setEasingCurve(QEasingCurve::InOutSine);
	glowAnimation_->setLoopCount(-1);
	connect(glowAnimation_, &QVariantAnimation::valueChanged, this, &OBSHeader::applyGlowProgress);

	// Connect to OBSBasic signals
	connect(main_, &OBSBasic::StreamingStarted, this, &OBSHeader::onStreamStarted);
	connect(main_, &OBSBasic::StreamingStopped, this, &OBSHeader::onStreamStopped);
	connect(main_, &OBSBasic::RecordingStarted, this, &OBSHeader::onRecordStarted);
	connect(main_, &OBSBasic::RecordingStopped, this, &OBSHeader::onRecordStopped);
	connect(main_, &OBSBasic::RecordingPaused,  this, &OBSHeader::onRecordPaused);
	connect(main_, &OBSBasic::RecordingUnpaused,this, &OBSHeader::onRecordUnpaused);
	connect(main_, &OBSBasic::VirtualCamStarted,this, &OBSHeader::onVCamStarted);
	connect(main_, &OBSBasic::VirtualCamStopped,this, &OBSHeader::onVCamStopped);

	// Button actions
	connect(btnStream_, &QPushButton::clicked, this, &OBSHeader::streamClicked);
	connect(btnRecord_, &QPushButton::clicked, this, &OBSHeader::recordClicked);
	connect(btnRecordPause_, &QPushButton::clicked, this, &OBSHeader::onPauseRecordClicked);
	connect(btnVCam_,   &QPushButton::clicked, this, &OBSHeader::vcamClicked);

	recordTimer_ = new QTimer(this);
	connect(recordTimer_, &QTimer::timeout, this, &OBSHeader::updateRecordTimer);
}

// -------------------------------------------------------
// Build UI (Bar format placed above the video preview)
// -------------------------------------------------------
void OBSHeader::buildUI()
{
	auto *layout = new QGridLayout(this);
	layout->setContentsMargins(12, 4, 12, 4);
	layout->setSpacing(0);

	// 1. Controls Container (Stream, Record, Pause, VCam)
	auto *controlsWidget = new QWidget(this);
	auto *controlsLayout = new QHBoxLayout(controlsWidget);
	controlsLayout->setContentsMargins(0, 0, 0, 0);
	controlsLayout->setSpacing(8);

	btnStream_ = new HeaderIconButton(":/res/images/lucide/radio-tower.svg", controlsWidget);
	btnStream_->setObjectName("btnStream");
	btnStream_->setAccessibleName(QString::fromUtf8("بدء البث المباشر"));
	btnStream_->setToolTip(QString::fromUtf8("بدء البث المباشر"));
	btnStream_->setFixedSize(42, 34);
	btnStream_->setProperty("active", false);
	btnStream_->setCursor(Qt::PointingHandCursor);
	controlsLayout->addWidget(btnStream_);

	// 2. Record button
	btnRecord_ = new HeaderIconButton(":/res/images/lucide/circle-dot.svg", controlsWidget);
	btnRecord_->setObjectName("btnRecord");
	btnRecord_->setAccessibleName(QString::fromUtf8("بدء التسجيل"));
	btnRecord_->setToolTip(QString::fromUtf8("بدء التسجيل"));
	btnRecord_->setFixedSize(42, 34);
	btnRecord_->setProperty("active", false);
	btnRecord_->setProperty("paused", false);
	btnRecord_->setCursor(Qt::PointingHandCursor);
	controlsLayout->addWidget(btnRecord_);

	// 2b. Record Pause button (visible only during recording)
	btnRecordPause_ = new HeaderIconButton(":/res/images/lucide/pause.svg", controlsWidget);
	btnRecordPause_->setObjectName("btnRecordPause");
	btnRecordPause_->setAccessibleName(QString::fromUtf8("إيقاف التسجيل مؤقتًا"));
	btnRecordPause_->setToolTip(QString::fromUtf8("إيقاف التسجيل مؤقتًا"));
	btnRecordPause_->setFixedSize(36, 34);
	btnRecordPause_->setCursor(Qt::PointingHandCursor);
	btnRecordPause_->setVisible(false);
	controlsLayout->addWidget(btnRecordPause_);

	// 2c. Record Time counter (visible only during recording)
	lblRecordTime_ = new QLabel("00:00:00", controlsWidget);
	lblRecordTime_->setObjectName("lblRecordTime");
	lblRecordTime_->setAlignment(Qt::AlignCenter);
	lblRecordTime_->setVisible(false);
	controlsLayout->addWidget(lblRecordTime_);

	// 3. Virtual Camera button
	btnVCam_ = new HeaderIconButton(":/res/images/lucide/video.svg", controlsWidget);
	btnVCam_->setObjectName("btnVCam");
	btnVCam_->setAccessibleName(QString::fromUtf8("تشغيل الكاميرا الافتراضية"));
	btnVCam_->setToolTip(QString::fromUtf8("تشغيل الكاميرا الافتراضية"));
	btnVCam_->setFixedSize(42, 34);
	btnVCam_->setProperty("active", false);
	btnVCam_->setCursor(Qt::PointingHandCursor);
	controlsLayout->addWidget(btnVCam_);

	// 4. Center Container (Titles dropdown + Gear)
	centerContainer_ = new QWidget(this);
	centerLayout_ = new QHBoxLayout(centerContainer_);
	centerLayout_->setContentsMargins(0, 0, 0, 0);
	centerLayout_->setSpacing(6);

	// 5. Balancer widget (equal stretch to ensure Column 1 stays in the absolute center)
	auto *balancerWidget = new QWidget(this);
	balancerWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

	layout->addWidget(controlsWidget, 0, 0, Qt::AlignVCenter | (isRightToLeft() ? Qt::AlignRight : Qt::AlignLeft));
	layout->addWidget(centerContainer_, 0, 1, Qt::AlignCenter);
	layout->addWidget(balancerWidget, 0, 2, Qt::AlignVCenter | (isRightToLeft() ? Qt::AlignLeft : Qt::AlignRight));

	layout->setColumnStretch(0, 1);
	layout->setColumnStretch(1, 0);
	layout->setColumnStretch(2, 1);
}

// -------------------------------------------------------
// Base Stylesheet
// -------------------------------------------------------
void OBSHeader::applyStylesheet()
{
	setStyleSheet(QString());
}

void OBSHeader::addTitleSelector(QComboBox *selector)
{
	if (!selector || !centerLayout_) return;

	selector->setParent(centerContainer_);
	selector->setLayoutDirection(Qt::RightToLeft);
	selector->setMinimumWidth(140);
	selector->setMaximumWidth(275);
	selector->view()->setLayoutDirection(Qt::RightToLeft);
	selector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	selector->setFixedHeight(34);
	selector->setAccessibleName(QString::fromUtf8("اختيار العنوان"));
	selector->setToolTip(selector->currentText().isEmpty()
				     ? QString::fromUtf8("اختيار عنوان محفوظ أو كتابة عنوان مؤقت")
				     : selector->currentText());

	// Lesson Number container (Number-only box + frameless + and - on its left)
	lessonNumberBox_ = new QWidget(centerContainer_);
	lessonNumberBox_->setObjectName("lessonNumberBox");
	lessonNumberBox_->setLayoutDirection(Qt::RightToLeft);
	auto *numLayout = new QHBoxLayout(lessonNumberBox_);
	numLayout->setContentsMargins(0, 0, 0, 0);
	numLayout->setSpacing(2);

	// Number-only box (without prefix or internal spin arrows)
	spinLessonNumber_ = new QSpinBox(lessonNumberBox_);
	spinLessonNumber_->setObjectName("spinLessonHeader");
	spinLessonNumber_->setLayoutDirection(Qt::RightToLeft);
	spinLessonNumber_->setRange(1, 99999);
	spinLessonNumber_->setValue(1);
	spinLessonNumber_->setButtonSymbols(QAbstractSpinBox::NoButtons);
	spinLessonNumber_->setAlignment(Qt::AlignCenter);
	spinLessonNumber_->setFixedHeight(34);
	spinLessonNumber_->setFixedWidth(52);
	spinLessonNumber_->setCursor(Qt::IBeamCursor);
	spinLessonNumber_->setToolTip(QString::fromUtf8("رقم الدرس"));
	connect(spinLessonNumber_, QOverload<int>::of(&QSpinBox::valueChanged), this, &OBSHeader::onSpinLessonValueChanged);

	// + button (frameless)
	btnIncrementLesson_ = new QPushButton(QString::fromUtf8("+"), lessonNumberBox_);
	btnIncrementLesson_->setObjectName("btnIncLessonHeader");
	btnIncrementLesson_->setFixedHeight(34);
	btnIncrementLesson_->setFixedWidth(24);
	btnIncrementLesson_->setCursor(Qt::PointingHandCursor);
	btnIncrementLesson_->setToolTip(QString::fromUtf8("زيادة رقم الدرس (+)"));
	connect(btnIncrementLesson_, &QPushButton::clicked, this, &OBSHeader::onBtnIncLessonClicked);

	// - button (frameless)
	btnDecrementLesson_ = new QPushButton(QString::fromUtf8("−"), lessonNumberBox_);
	btnDecrementLesson_->setObjectName("btnDecLessonHeader");
	btnDecrementLesson_->setFixedHeight(34);
	btnDecrementLesson_->setFixedWidth(24);
	btnDecrementLesson_->setCursor(Qt::PointingHandCursor);
	btnDecrementLesson_->setToolTip(QString::fromUtf8("إنقاص رقم الدرس (−)"));
	connect(btnDecrementLesson_, &QPushButton::clicked, this, &OBSHeader::onBtnDecLessonClicked);

	// In RTL: first added is on the right, so number box is on the right, and + then - are on its left
	numLayout->addWidget(spinLessonNumber_);
	numLayout->addWidget(btnIncrementLesson_);
	numLayout->addWidget(btnDecrementLesson_);

	lessonNumberBox_->setVisible(false);

	btnManageTitles_ = new HeaderIconButton(":/res/images/lucide/settings.svg", centerContainer_);
	btnManageTitles_->setObjectName("btnManageTitles");
	btnManageTitles_->setAccessibleName(QString::fromUtf8("إدارة العناوين"));
	btnManageTitles_->setToolTip(QString::fromUtf8("إدارة العناوين وسلاسل الدروس والخطوط"));
	btnManageTitles_->setFixedSize(34, 34);
	btnManageTitles_->setCursor(Qt::PointingHandCursor);
	connect(btnManageTitles_, &QPushButton::clicked, main_, &OBSBasic::OpenTitleManager);

	// In RTL, selector is added first, then lesson number box, then gear
	centerLayout_->addWidget(selector);
	centerLayout_->addWidget(lessonNumberBox_);
	centerLayout_->addWidget(btnManageTitles_);
}

void OBSHeader::setLessonNumber(int num)
{
	if (!spinLessonNumber_) return;
	spinLessonNumber_->blockSignals(true);
	spinLessonNumber_->setValue(num);
	spinLessonNumber_->blockSignals(false);
}

void OBSHeader::setLessonNumberVisible(bool visible)
{
	if (lessonNumberBox_) {
		lessonNumberBox_->setVisible(visible);
	}
}

int OBSHeader::lessonNumber() const
{
	return spinLessonNumber_ ? spinLessonNumber_->value() : 1;
}

void OBSHeader::onSpinLessonValueChanged(int val)
{
	emit lessonNumberChanged(val);
}

void OBSHeader::onBtnIncLessonClicked()
{
	if (spinLessonNumber_)
		spinLessonNumber_->setValue(spinLessonNumber_->value() + 1);
}

void OBSHeader::onBtnDecLessonClicked()
{
	if (spinLessonNumber_ && spinLessonNumber_->value() > 1)
		spinLessonNumber_->setValue(spinLessonNumber_->value() - 1);
}

// -------------------------------------------------------
// Animated icon halo; button geometry never changes.
// -------------------------------------------------------
void OBSHeader::updateActiveGlow()
{
	const bool active = streaming_ || recording_ || vcam_;
	if (active && glowAnimation_->state() != QAbstractAnimation::Running)
		glowAnimation_->start();
	else if (!active && glowAnimation_->state() == QAbstractAnimation::Running)
		glowAnimation_->stop();
	if (!active) {
		glowProgress_ = 0.0;
		applyGlowProgress(0.0);
	}
}

void OBSHeader::applyGlowProgress(const QVariant &value)
{
	glowProgress_ = value.toReal();
	const QColor fallback = palette().color(QPalette::Highlight);
	const QColor streamColor = btnStream_ ? btnStream_->palette().color(QPalette::Button) : fallback;
	const QColor recordColor = btnRecord_ ? btnRecord_->palette().color(QPalette::Button) : fallback;
	const QColor cameraColor = btnVCam_ ? btnVCam_->palette().color(QPalette::Button) : fallback;
	if (auto *button = dynamic_cast<HeaderIconButton *>(btnStream_))
		button->setGlow(streaming_ ? streamColor : QColor(), glowProgress_);
	if (auto *button = dynamic_cast<HeaderIconButton *>(btnRecord_))
		button->setGlow(recording_ ? recordColor : QColor(), glowProgress_);
	if (auto *button = dynamic_cast<HeaderIconButton *>(btnVCam_))
		button->setGlow(vcam_ ? cameraColor : QColor(), glowProgress_);
}

// -------------------------------------------------------
// Stream state
// -------------------------------------------------------
void OBSHeader::onStreamStarted()
{
	streaming_ = true;
	btnStream_->setAccessibleName("إيقاف البث المباشر");
	btnStream_->setToolTip("إيقاف البث المباشر");
	btnStream_->setProperty("active", true);
	RefreshButtonStyle(btnStream_);

	// Query active platforms from MultiStreamManager
	QStringList activeIds = MultiStreamManager::Instance().GetActiveSessionIds();
	if (activeIds.isEmpty()) {
		auto targets = MultiStreamManager::Instance().GetConfiguredTargets();
		for (const auto &t : targets) {
			activeIds.append(t.id);
		}
	}
	if (activeIds.isEmpty()) {
		activeIds.append("youtube");
	}

	QStringList platformNames;
	if (activeIds.contains("youtube", Qt::CaseInsensitive)) platformNames.append("YouTube");
	if (activeIds.contains("facebook", Qt::CaseInsensitive)) platformNames.append("Facebook");
	if (activeIds.contains("instagram", Qt::CaseInsensitive)) platformNames.append("Instagram");
	if (!platformNames.isEmpty())
		btnStream_->setToolTip(QStringLiteral("إيقاف البث المباشر · %1").arg(platformNames.join("، ")));

	updateActiveGlow();
}

void OBSHeader::onStreamStopped()
{
	streaming_ = false;
	btnStream_->setAccessibleName("بدء البث المباشر");
	btnStream_->setToolTip("بدء البث المباشر");
	btnStream_->setProperty("active", false);
	RefreshButtonStyle(btnStream_);

	updateActiveGlow();
}

// -------------------------------------------------------
// Record state
// -------------------------------------------------------
void OBSHeader::onRecordStarted()
{
	recording_ = true;
	recordingPaused_ = false;
	recordSeconds_ = 0;
	if (lblRecordTime_) {
		lblRecordTime_->setText("00:00:00");
		lblRecordTime_->setVisible(true);
	}
	if (recordTimer_) {
		recordTimer_->start(1000);
	}
	btnRecord_->setAccessibleName("إيقاف التسجيل");
	btnRecord_->setToolTip("إيقاف التسجيل");
	btnRecord_->setProperty("active", true);
	btnRecord_->setProperty("paused", false);
	RefreshButtonStyle(btnRecord_);
	btnRecordPause_->setProperty("paused", false);
	btnRecordPause_->setAccessibleName("إيقاف التسجيل مؤقتًا");
	btnRecordPause_->setToolTip("إيقاف التسجيل مؤقتًا");
	btnRecordPause_->setProperty("recording", true);
	RefreshButtonStyle(btnRecordPause_);
	btnRecordPause_->setVisible(true);
	updateActiveGlow();
}

void OBSHeader::onRecordStopped()
{
	recording_ = false;
	recordingPaused_ = false;
	if (recordTimer_) {
		recordTimer_->stop();
	}
	recordSeconds_ = 0;
	if (lblRecordTime_) {
		lblRecordTime_->setVisible(false);
		lblRecordTime_->setText("00:00:00");
	}
	btnRecord_->setAccessibleName("بدء التسجيل");
	btnRecord_->setToolTip("بدء التسجيل");
	btnRecord_->setProperty("active", false);
	btnRecord_->setProperty("paused", false);
	RefreshButtonStyle(btnRecord_);
	btnRecordPause_->setProperty("recording", false);
	btnRecordPause_->setVisible(false);
	RefreshButtonStyle(btnRecordPause_);

	updateActiveGlow();
}

void OBSHeader::onRecordPaused()
{
	recordingPaused_ = true;
	if (recordTimer_) {
		recordTimer_->stop();
	}
	SetHeaderButtonIcon(btnRecordPause_, ":/res/images/lucide/play.svg");
	btnRecordPause_->setProperty("paused", true);
	btnRecordPause_->setAccessibleName("استئناف التسجيل");
	btnRecordPause_->setToolTip("استئناف التسجيل");
	btnRecord_->setProperty("paused", true);
	RefreshButtonStyle(btnRecord_);
	RefreshButtonStyle(btnRecordPause_);
	updateActiveGlow();
}

void OBSHeader::onRecordUnpaused()
{
	recordingPaused_ = false;
	if (recordTimer_) {
		recordTimer_->start(1000);
	}
	SetHeaderButtonIcon(btnRecordPause_, ":/res/images/lucide/pause.svg");
	btnRecordPause_->setProperty("paused", false);
	btnRecordPause_->setAccessibleName("إيقاف التسجيل مؤقتًا");
	btnRecordPause_->setToolTip("إيقاف التسجيل مؤقتًا");
	btnRecord_->setProperty("paused", false);
	RefreshButtonStyle(btnRecord_);
	RefreshButtonStyle(btnRecordPause_);
	updateActiveGlow();
}

void OBSHeader::updateRecordTimer()
{
	if (!recording_ || recordingPaused_) return;
	recordSeconds_++;
	int s = recordSeconds_ % 60;
	int m = (recordSeconds_ / 60) % 60;
	int h = recordSeconds_ / 3600;
	if (lblRecordTime_) {
		lblRecordTime_->setText(QString::asprintf("%02d:%02d:%02d", h, m, s));
	}
}

void OBSHeader::onPauseRecordClicked()
{
	if (!main_) return;

	/* The header is updated by the RecordingPaused / RecordingUnpaused
	 * signals, which OBSBasic only emits when libobs accepted the change.
	 * Flipping the state here as well left the button showing "resume"
	 * while the output was still recording (or the opposite) whenever the
	 * pause request was refused, and the next click then did nothing. */
	if (!recordingPaused_) {
		main_->PauseRecording();
	} else {
		main_->UnpauseRecording();
	}
}

// -------------------------------------------------------
// VCam state
// -------------------------------------------------------
void OBSHeader::onVCamStarted()
{
	vcam_ = true;
	btnVCam_->setAccessibleName("إيقاف الكاميرا الافتراضية");
	btnVCam_->setToolTip("إيقاف الكاميرا الافتراضية");
	btnVCam_->setProperty("active", true);
	RefreshButtonStyle(btnVCam_);
	updateActiveGlow();
}

void OBSHeader::onVCamStopped()
{
	vcam_ = false;
	btnVCam_->setAccessibleName("تشغيل الكاميرا الافتراضية");
	btnVCam_->setToolTip("تشغيل الكاميرا الافتراضية");
	btnVCam_->setProperty("active", false);
	RefreshButtonStyle(btnVCam_);

	updateActiveGlow();
}

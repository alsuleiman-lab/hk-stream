#pragma once
#include <QWidget>
#include <QPushButton>
#include <QSpinBox>

class QVariantAnimation;
class QVariant;
class QHBoxLayout;
class OBSBasic;
class QComboBox;
class QLabel;
class QTimer;

// -------------------------------------------------------
// OBSHeader — Top bar above the video with controls & titles
// -------------------------------------------------------
class OBSHeader : public QWidget {
	Q_OBJECT

public:
	explicit OBSHeader(OBSBasic *main, QWidget *parent = nullptr);
	~OBSHeader() override = default;
	void addTitleSelector(QComboBox *selector);

	void setLessonNumber(int num);
	void setLessonNumberVisible(bool visible);
	int lessonNumber() const;

public slots:
	void onStreamStarted();
	void onStreamStopped();
	void onRecordStarted();
	void onRecordStopped();
	void onRecordPaused();
	void onRecordUnpaused();
	void onVCamStarted();
	void onVCamStopped();

signals:
	void streamClicked();
	void recordClicked();
	void vcamClicked();
	void lessonNumberChanged(int num);

private slots:
	void onPauseRecordClicked();
	void updateActiveGlow();
	void applyGlowProgress(const QVariant &value);
	void onSpinLessonValueChanged(int val);
	void onBtnIncLessonClicked();
	void onBtnDecLessonClicked();
	void updateRecordTimer();

private:
	void buildUI();
	void applyStylesheet();

	OBSBasic *main_ = nullptr;

	QPushButton *btnStream_             = nullptr;
	QPushButton *btnRecord_             = nullptr;
	QPushButton *btnRecordPause_        = nullptr;
	QLabel      *lblRecordTime_         = nullptr;
	QPushButton *btnVCam_               = nullptr;
	QPushButton *btnManageTitles_       = nullptr;

	QTimer      *recordTimer_           = nullptr;
	int          recordSeconds_         = 0;

	QWidget     *centerContainer_       = nullptr;
	QHBoxLayout *centerLayout_          = nullptr;

	// Lesson Number Controls
	QWidget     *lessonNumberBox_       = nullptr;
	QSpinBox    *spinLessonNumber_      = nullptr;
	QPushButton *btnIncrementLesson_    = nullptr;
	QPushButton *btnDecrementLesson_    = nullptr;

	QVariantAnimation *glowAnimation_   = nullptr;
	qreal glowProgress_ = 0.0;

	// State
	bool streaming_        = false;
	bool recording_        = false;
	bool recordingPaused_  = false;
	bool vcam_             = false;
};

#pragma once

#include <QDialog>
#include <QStringList>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFontComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>

class OBSBasic;

struct TitleEntry {
	QString baseTitle;
	bool hasNumber = false;
	int lessonNumber = 1;
	QString separator = " / ";
	QString fontFace = "zain mob-long300";
	int fontSize = 120;
	bool isBold = false;
	bool isImage = false;
	QString imagePath;
	QString text;

	QString fullTitle() const
	{
		if (!hasNumber) return baseTitle.isEmpty() ? text : baseTitle;
		QString sep = separator;
		if (sep.isEmpty()) sep = " ";
		return QString("%1%2%3").arg(baseTitle.trimmed(), sep, QString::number(lessonNumber)).trimmed();
	}
};

class TitleManagerDialog : public QDialog {
	Q_OBJECT

public:
	explicit TitleManagerDialog(OBSBasic *main, QWidget *parent = nullptr);
	~TitleManagerDialog() override = default;

	static QList<TitleEntry> LoadTitlesData();
	static QStringList LoadTitlesList();
	static bool SaveTitlesData(const QList<TitleEntry> &entries, const QString &activeTitle);
	static QString GetActiveTitle();
	static TitleEntry GetActiveTitleEntry();
	static QString GetTemporaryTitle();
	static void GetTemporaryTitleFont(QString &fontFace, int &fontSize, bool &isBold);
	static void SaveTemporaryTitleFont(const QString &fontFace, int fontSize, bool isBold);
	static void ParseTitleString(const QString &raw, TitleEntry &entry);
	static bool UpdateLessonNumberForActive(int newLessonNumber);
	static bool IncrementActiveLessonNumber(int delta);

	static void ApplyTemporaryTitleToOBS(const QString &titleText,
	                                     const QString &fontFace = QString(),
	                                     int fontSize = 0,
	                                     bool isBold = false);
	static void RestoreTemporaryTitleFromOBS();
	static bool SuspendTemporaryTitleForSave();
	static void ResumeTemporaryTitleAfterSave();
	static void ApplyTitleToOBS(const QString &titleText,
	                            const QString &fontFace = QString(),
	                            int fontSize = 0,
	                            bool isBold = false,
	                            bool tightBounds = true);

signals:
	void titleChanged(const QString &newTitle);

private slots:
	void onTitleSelected(int index);
	void onAddTitleClicked();
	void onAddImageTitleClicked();
	void onDeleteTitleClicked();
	void onApplyClicked();
	void onFontSettingsChanged();
	void onBrowseFontClicked();
	void onApplyTempClicked();
	void onCancelTempClicked();
	void onTempFontSettingsChanged();
	void onBrowseTempFontClicked();
	void onLessonNumberChanged();
	void onIncrementLessonClicked();
	void onDecrementLessonClicked();
	void updateFullPreview();

private:
	void buildUI();
	void applyStylesheet();
	void refreshList();
	TitleEntry *findEntry(const QString &text);

	OBSBasic *main_ = nullptr;
	QString activeTitle_;
	QList<TitleEntry> titlesData_;

	QLabel *lblCurrentActive_ = nullptr;
	QComboBox *comboTitles_ = nullptr;
	TitleEntry *currentSelectedEntry();

	// Specialized Title & Lesson Details Panel
	QGroupBox *groupTitleDetails_ = nullptr;
	QLineEdit *txtEditBaseTitle_ = nullptr;
	QCheckBox *chkHasNumber_ = nullptr;
	QSpinBox *spinLessonNum_ = nullptr;
	QPushButton *btnIncLesson_ = nullptr;
	QPushButton *btnDecLesson_ = nullptr;
	QComboBox *comboSeparator_ = nullptr;
	QLabel *lblFullPreview_ = nullptr;

	QFontComboBox *comboFont_ = nullptr;
	QCheckBox *chkBold_ = nullptr;
	QPushButton *btnBrowseFont_ = nullptr;
	QCheckBox *chkTightBounds_ = nullptr;
	QPushButton *btnSnapFrame_ = nullptr;

	// Temporary Title Section
	QGroupBox *groupTemp_ = nullptr;
	QLineEdit *txtTempTitle_ = nullptr;
	QFontComboBox *comboTempFont_ = nullptr;
	QCheckBox *chkTempBold_ = nullptr;
	QPushButton *btnBrowseTempFont_ = nullptr;
	QPushButton *btnApplyTemp_ = nullptr;
	QPushButton *btnCancelTemp_ = nullptr;

	// Add New Title Section
	QLineEdit *txtNewTitle_ = nullptr;
	QCheckBox *chkNewHasNumber_ = nullptr;
	QSpinBox *spinNewLesson_ = nullptr;
	QFontComboBox *comboNewFont_ = nullptr;
	QCheckBox *chkNewBold_ = nullptr;
	QPushButton *btnAdd_ = nullptr;
	QPushButton *btnAddImageTitle_ = nullptr;

	QPushButton *btnDelete_ = nullptr;
	QPushButton *btnApply_ = nullptr;
	QPushButton *btnClose_ = nullptr;
};

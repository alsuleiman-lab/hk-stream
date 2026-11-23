#pragma once

#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QSpinBox>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <obs.hpp>

class OBSBasic;
class OBSQTDisplay;

class WebSourceCodeDialog : public QDialog {
	Q_OBJECT

public:
	explicit WebSourceCodeDialog(obs_source_t *source, OBSBasic *main, QWidget *parent = nullptr);
	~WebSourceCodeDialog() override;

private slots:
	void onCodeChanged();
	void onRefreshPreview();
	void onApplyAndClose();
	void onPresetChanged(int index);
	void onOpenAdvancedProperties();

private:
	void buildUI();
	void loadSourceCode();
	bool updatePreviewSource();
	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);

	OBSSource source_;
	OBSBasic *main_ = nullptr;

	QPlainTextEdit *codeEditor_ = nullptr;
	OBSQTDisplay *previewDisplay_ = nullptr;
	QLabel *lblStatus_ = nullptr;
	QComboBox *comboPreset_ = nullptr;

	QSpinBox *spinWidth_ = nullptr;
	QSpinBox *spinHeight_ = nullptr;

	QPushButton *btnRefresh_ = nullptr;
	QPushButton *btnSave_ = nullptr;
	QPushButton *btnCancel_ = nullptr;
	QPushButton *btnAdvProps_ = nullptr;

	QString internalHtmlPath_;
	bool isModified_ = false;
};

#pragma once

#include <QDialog>
#include <QIcon>
#include <QString>
#include <vector>
#include <cstdint>

class QListWidget;
class QListWidgetItem;
class QPushButton;

struct ResolutionChoice {
	uint32_t cx = 1920;
	uint32_t cy = 1080;
	QString title;
};

class RecordingResolutionDialog : public QDialog {
	Q_OBJECT

public:
	explicit RecordingResolutionDialog(uint32_t currentCX, uint32_t currentCY, QWidget *parent = nullptr);
	~RecordingResolutionDialog() override = default;

	uint32_t GetSelectedCX() const { return selectedCX_; }
	uint32_t GetSelectedCY() const { return selectedCY_; }
	const std::vector<ResolutionChoice> &GetSelectedResolutions() const { return selectedResolutions_; }
	bool IsSimultaneousMultiRecord() const { return selectedResolutions_.size() > 1; }

	static QIcon MakeAspectRatioIcon(int wRatio, int hRatio, int iconSize = 28);

private slots:
	void OnConfirm();
	void OnItemClicked(QListWidgetItem *item);
	void OnItemChanged(QListWidgetItem *item);

private:
	void BuildUI();
	void ApplyStyles();
	void UpdateConfirmButton();

	uint32_t currentCX_ = 1920;
	uint32_t currentCY_ = 1080;
	uint32_t selectedCX_ = 1920;
	uint32_t selectedCY_ = 1080;

	QListWidget *listWidget_ = nullptr;
	QPushButton *btnConfirm_ = nullptr;
	std::vector<ResolutionChoice> selectedResolutions_;
	bool updatingChecks_ = false;
};

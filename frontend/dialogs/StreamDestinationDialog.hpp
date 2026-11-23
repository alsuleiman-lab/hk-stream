#pragma once

#include <QDialog>
#include <QList>
#include <QMap>
#include <QSize>
#include <QStringList>
#include "utility/MultiStreamManager.hpp"

class QCheckBox;
class QPushButton;
class QComboBox;
class QVBoxLayout;

class StreamDestinationDialog : public QDialog {
	Q_OBJECT

public:
	explicit StreamDestinationDialog(const QList<MultiStreamTarget> &targets, uint32_t currentCX,
				 uint32_t currentCY, QWidget *parent = nullptr);
	~StreamDestinationDialog() override = default;

	QStringList GetSelectedTargetIds() const;
	QMap<QString, QSize> GetSelectedResolutions() const;

public slots:
	void accept() override;

private slots:
	void OnSelectAllClicked();
	void OnDeselectAllClicked();
	void OnCheckboxToggled();

private:
	void SetupUI(const QList<MultiStreamTarget> &targets, uint32_t currentCX, uint32_t currentCY);

	QMap<QString, QCheckBox *> checkBoxes_;
	QMap<QString, QComboBox *> resolutionCombos_;
	QStringList targetOrder_;
	QPushButton *btnStart_ = nullptr;
	QPushButton *btnCancel_ = nullptr;
};

#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>

class RecycleBinDialog : public QDialog {
	Q_OBJECT

public:
	explicit RecycleBinDialog(QWidget *parent = nullptr);
	~RecycleBinDialog() override = default;

private slots:
	void RefreshList();
	void OnRestoreClicked(const QString &id);
	void OnDeleteClicked(const QString &id);
	void OnClearAllClicked();
	void OnFilterChanged(int id);

private:
	void BuildUI();
	void ApplyStyles();

	QListWidget *listWidget_ = nullptr;
	QLabel *lblCount_ = nullptr;
	QButtonGroup *filterGroup_ = nullptr;
	int currentFilter_ = 0; // 0: All, 1: Scenes, 2: Sources
};

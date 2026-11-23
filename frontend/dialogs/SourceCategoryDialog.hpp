#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QString>
#include <vector>
#include <string>

class OBSBasic;
class OBSWorkspaceManager;

class SourceCategoryDialog : public QDialog {
	Q_OBJECT

public:
	explicit SourceCategoryDialog(OBSWorkspaceManager *mgr, OBSBasic *main, QWidget *parent = nullptr);
	~SourceCategoryDialog() override = default;

private slots:
	void onMoveToCore();
	void onMoveToVariable();
	void onRemoveCategory();
	void onSaveClicked();

private:
	void buildUI();
	void applyStylesheet();
	void populateLists();

	OBSWorkspaceManager *mgr_ = nullptr;
	OBSBasic *main_           = nullptr;
	std::string wsId_;

	QLabel *lblWsName_           = nullptr;
	QListWidget *listCore_       = nullptr;
	QListWidget *listVariable_   = nullptr;
	QListWidget *listAvailable_  = nullptr;

	QPushButton *btnMoveToCore_     = nullptr;
	QPushButton *btnMoveToVariable_ = nullptr;
	QPushButton *btnRemoveCategory_ = nullptr;

	QPushButton *btnSave_   = nullptr;
	QPushButton *btnCancel_ = nullptr;
};

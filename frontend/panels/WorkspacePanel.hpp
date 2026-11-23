#pragma once

#include <QWidget>
#include <QPushButton>
#include <QListWidget>
#include <QComboBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <memory>
#include <string>

class OBSBasic;
class OBSWorkspaceManager;
struct OBSWorkspace;
namespace idian { class ToggleSwitch; }

class WorkspacePanel : public QWidget {
	Q_OBJECT

public:
	explicit WorkspacePanel(OBSWorkspaceManager *mgr, OBSBasic *main = nullptr, QWidget *parent = nullptr);
	~WorkspacePanel() override;

	void SetWorkspaceManager(OBSWorkspaceManager *mgr);
	void RefreshAll();
	void UpdateSmartDirectorConfig();
	void CaptureActiveSceneLayout();
	void ApplySceneLayout(const std::string &aspect_key);

signals:
	void workspaceChanged(const QString &id);

public slots:
	void OnWorkspaceSelected(const QString &id);
	void OnResolutionChanged(int index);
	void OnSceneClicked(QListWidgetItem *item);
	void OnOBSSceneChanged();

	// Scene Management Slots
	void OnAddScene();
	void OnRemoveScene();
	void OnReorderScene(int fromRow, int toRow);

	// Duplicate, Recycle Bin, and Hidden Scenes
	void OnDuplicateWorkspace();
	void OnOpenRecycleBin();
	void OnToggleShowHidden();

private:
	void BuildUI();
	void ApplyStyles();
	void UpdateWorkspaceButtons();
	void UpdateResolutionDisplay();
	void UpdateSceneList();
	void ActivateCurrentWorkspaceScene();

	void CaptureCurrentSceneLayout(const std::string &aspect_key);
	void ApplyCurrentSceneLayout(const std::string &aspect_key);
	void OnInteractiveModeToggled(bool enabled);

	OBSWorkspaceManager *mgr_ = nullptr;
	OBSBasic *main_ = nullptr;

	// UI Elements
	QLabel *lblInteractiveMode_ = nullptr;
	idian::ToggleSwitch *interactiveModeToggle_ = nullptr;
	QComboBox *comboWorkspace_ = nullptr;

	QComboBox *comboRes_ = nullptr;
	QListWidget *sceneList_ = nullptr;

	// Scene Action Buttons
	QPushButton *btnAddScene_     = nullptr;
	QPushButton *btnRemoveScene_  = nullptr;
	QPushButton *btnRecycleBin_   = nullptr;
	QPushButton *btnDuplicateWs_  = nullptr;
	QPushButton *btnToggleHidden_ = nullptr;

	bool showHiddenScenes_ = false;
	bool updatingSelection_ = false;
	bool updatingSceneContext_ = false;
};

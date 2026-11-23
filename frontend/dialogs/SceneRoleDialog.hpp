#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QRadioButton>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>

class SceneRoleDialog : public QDialog {
	Q_OBJECT

public:
	enum class Role {
		Normal,
		Base,
		Interactive
	};

	explicit SceneRoleDialog(const QString &sceneName,
				 const QString &workspaceId,
				 const QString &workspaceName,
				 Role currentRole,
				 const QString &currentSource,
				 const QString &currentFallbackScene,
				 const QStringList &availableSources,
				 const QStringList &availableFallbackScenes,
				 QWidget *parent = nullptr);

	Role GetSelectedRole() const;
	QString GetSelectedInteractiveSource() const;
	QString GetSelectedInteractiveFallbackScene() const;

	void SetInitialTextColors(const std::map<std::string, std::string> &colors);
	std::map<std::string, std::string> GetCustomTextColors() const;

private:
	QString sceneName_;
	QString workspaceId_;

	QRadioButton *rbNormal_ = nullptr;
	QRadioButton *rbBase_ = nullptr;
	QRadioButton *rbInteractive_ = nullptr;
	QWidget *sourceGroupWidget_ = nullptr;
	QComboBox *comboSources_ = nullptr;
	QComboBox *comboFallbackScenes_ = nullptr;
	QLabel *lblValidation_ = nullptr;
	QPushButton *btnSave_ = nullptr;
	QPushButton *btnCancel_ = nullptr;

	struct TextElementRowUI {
		std::string id;
		QString displayName;
		class QCheckBox *chkEnable = nullptr;
		QPushButton *btnColor = nullptr;
		QPushButton *btnReset = nullptr;
		QString currentColorHex;
		QString defaultColorHex;
	};
	std::vector<TextElementRowUI> textElementRows_;

	// 3 Color Modes
	QRadioButton *rbColorOriginal_ = nullptr;
	QRadioButton *rbColorUnified_ = nullptr;
	QRadioButton *rbColorCustom_ = nullptr;

	QWidget *widgetUnifiedColor_ = nullptr;
	QPushButton *btnUnifiedColor_ = nullptr;
	QString unifiedColorHex_ = "#ffffff";

	QWidget *widgetCustomColors_ = nullptr;

	void UpdateUIState();
	void UpdateColorSwatch(size_t index);
	void UpdateUnifiedColorButton();
	void UpdateColorModeUI();
};

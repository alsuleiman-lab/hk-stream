/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <algorithm>

#include <qt-wrappers.hpp>

#ifdef BROWSER_AVAILABLE
struct QCef;
extern QCef *cef;
#endif

void setupDockAction(QDockWidget *dock)
{
	QAction *action = dock->toggleViewAction();

	auto neverDisable = [action]() {
		QSignalBlocker block(action);
		action->setEnabled(true);
	};

	auto newToggleView = [dock](bool check) {
		QSignalBlocker block(dock);
		dock->setVisible(check);
	};

	// Replace the slot connected by default
	QObject::disconnect(action, &QAction::triggered, nullptr, 0);
	QObject::connect(action, &QAction::triggered, dock, newToggleView);

	// Make the action unable to be disabled
	QObject::connect(action, &QAction::enabledChanged, action, neverDisable);
}

void OBSBasic::TrackDockForPersistence(QDockWidget *dock)
{
	if (!dock || dock->property("hkDockPersistenceTracked").toBool())
		return;

	dock->setProperty("hkDockPersistenceTracked", true);
	dock->installEventFilter(this);
	connect(dock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea) {
		ScheduleDockStateSave();
	});
	connect(dock, &QDockWidget::topLevelChanged, this, [this](bool) { ScheduleDockStateSave(); });
	connect(dock, &QDockWidget::visibilityChanged, this, [this](bool) { ScheduleDockStateSave(); });
}

bool OBSBasic::eventFilter(QObject *watched, QEvent *event)
{
	if (dockStatePersistenceReady_ &&
	    (watched == this || qobject_cast<QDockWidget *>(watched)) &&
	    (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Move ||
	     event->type() == QEvent::Resize || event->type() == QEvent::Show ||
	     event->type() == QEvent::Hide || event->type() == QEvent::ParentChange)) {
		ScheduleDockStateSave();
	}

	return OBSMainWindow::eventFilter(watched, event);
}

void OBSBasic::ScheduleDockStateSave()
{
	if (dockStatePersistenceReady_ && dockStateSaveTimer_)
		dockStateSaveTimer_->start();
}

void OBSBasic::SaveDockState()
{
	if (!dockStatePersistenceReady_)
		return;

	const QByteArray state = saveState();
	const QByteArray encodedState = state.toBase64();
	const bool layoutChanged = state != lastSavedDockState_;
	bool changed = layoutChanged;

	if (dockStateRestoreFailed_ && layoutChanged) {
		config_t *userConfig = App()->GetUserConfig();
		if (!config_get_string(userConfig, "BasicWindow", "DockStateRecoveryBackup")) {
			const char *oldState = config_get_string(userConfig, "BasicWindow", "DockState");
			if (oldState)
				config_set_string(userConfig, "BasicWindow", "DockStateRecoveryBackup", oldState);
		}
		config_set_string(userConfig, "BasicWindow", "DockStateRecoveryReason",
				  "The saved dock layout could not be restored; original state was preserved.");
	}

	if (!dockStateRestoreFailed_ &&
	    config_get_int(App()->GetUserConfig(), "BasicWindow", "DockStateFormat") != 2)
		changed = true;

#ifdef BROWSER_AVAILABLE
	if (cef) {
		SaveExtraBrowserDocks();
		const char *savedBrowserDocks =
			config_get_string(App()->GetUserConfig(), "BasicWindow", "ExtraBrowserDocks");
		const QString currentBrowserDocks = QString::fromUtf8(savedBrowserDocks ? savedBrowserDocks : "");
		if (currentBrowserDocks != lastSavedExtraBrowserDocks_) {
			lastSavedExtraBrowserDocks_ = currentBrowserDocks;
			changed = true;
		}
	}
#endif

	if (layoutChanged || (!dockStateRestoreFailed_ && changed)) {
		config_set_string(App()->GetUserConfig(), "BasicWindow", "DockState", encodedState.constData());
		config_set_int(App()->GetUserConfig(), "BasicWindow", "DockStateFormat", 2);
		changed = true;
	}

	if (!changed)
		return;

	lastSavedDockState_ = state;
	if (layoutChanged)
		dockStateRestoreFailed_ = false;
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
}

void OBSBasic::ClampFloatingDockWidgets()
{
	const QList<QScreen *> screens = QGuiApplication::screens();
	if (screens.isEmpty())
		return;

	for (QDockWidget *dock : findChildren<QDockWidget *>()) {
		if (!dock->isFloating())
			continue;

		const QRect frame = dock->frameGeometry();
		QScreen *target = nullptr;
		int bestArea = 0;
		for (QScreen *screen : screens) {
			const QRect available = screen->availableGeometry();
			const int area = frame.intersected(available).width() * frame.intersected(available).height();
			if (area > bestArea) {
				bestArea = area;
				target = screen;
			}
		}
		if (!target)
			target = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen() : screens.first();

		const QRect available = target->availableGeometry();
		QRect bounded = frame;
		bounded.setWidth(std::min(bounded.width(), available.width()));
		bounded.setHeight(std::min(bounded.height(), available.height()));
		bounded.moveLeft(std::clamp(bounded.left(), available.left(),
					   available.right() - bounded.width() + 1));
		bounded.moveTop(std::clamp(bounded.top(), available.top(),
					  available.bottom() - bounded.height() + 1));

		if (bounded.topLeft() != frame.topLeft())
			dock->move(dock->pos() + bounded.topLeft() - frame.topLeft());
	}
}

void OBSBasic::on_resetDocks_triggered(bool force)
{
#ifdef BROWSER_AVAILABLE
	if ((extraDocks.size() || extraCustomDocks.size() || extraBrowserDocks.size()) && !force)
#else
	if ((extraDocks.size() || extraCustomDocks.size()) && !force)
#endif
	{
		QMessageBox::StandardButton button =
			OBSMessageBox::question(this, QTStr("ResetUIWarning.Title"), QTStr("ResetUIWarning.Text"));

		if (button == QMessageBox::No) {
			return;
		}
	}

#define RESET_DOCKLIST(dockList)                                                                               \
	for (int i = dockList.size() - 1; i >= 0; i--) {                                                       \
		dockList[i]->setVisible(true);                                                                 \
		dockList[i]->setFloating(true);                                                                \
		dockList[i]->move(frameGeometry().topLeft() + rect().center() - dockList[i]->rect().center()); \
		dockList[i]->setVisible(false);                                                                \
	}

	RESET_DOCKLIST(extraDocks)
	RESET_DOCKLIST(extraCustomDocks)
#ifdef BROWSER_AVAILABLE
	RESET_DOCKLIST(extraBrowserDocks)
#endif
#undef RESET_DOCKLIST

	restoreState(startingDockLayout);
	ui->sideDocks->setChecked(true);

	int cx = width();
	int bottomDocksHeight = height();

	bottomDocksHeight = bottomDocksHeight * 225 / 1000;

	removeDockWidget(ui->scenesDock);
	ui->scenesDock->setParent(nullptr);
	ui->scenesDock->hide();
	ui->scenesDock->toggleViewAction()->setVisible(false);
	ui->scenesDock->toggleViewAction()->setEnabled(false);
	if (workspaceDock)
		workspaceDock->setVisible(true);
	ui->sourcesDock->setVisible(true);
	ui->mixerDock->setVisible(true);
	ui->transitionsDock->setVisible(true);
	controlsDock->setVisible(true);
	statsDock->setVisible(false);
	statsDock->setFloating(true);

	QList<QDockWidget *> bottomDocks{ui->mixerDock, ui->transitionsDock, controlsDock};

	resizeDocks(bottomDocks, {bottomDocksHeight, bottomDocksHeight, bottomDocksHeight}, Qt::Vertical);
	resizeDocks(bottomDocks, {cx * 45 / 100, cx * 14 / 100, cx * 16 / 100}, Qt::Horizontal);

	int sideDockWidth = std::min(width() * 30 / 100, 280);
	if (workspaceDock)
		resizeDocks({workspaceDock, ui->sourcesDock}, {sideDockWidth, sideDockWidth}, Qt::Horizontal);

	activateWindow();
	dockStateRestoreFailed_ = false;
	ScheduleDockStateSave();
}

void OBSBasic::on_lockDocks_toggled(bool lock)
{
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	QDockWidget::DockWidgetFeatures mainFeatures = features;
	mainFeatures &= ~QDockWidget::QDockWidget::DockWidgetClosable;

	ui->scenesDock->setFeatures(mainFeatures);
	ui->sourcesDock->setFeatures(mainFeatures);
	ui->mixerDock->setFeatures(mainFeatures);
	ui->transitionsDock->setFeatures(mainFeatures);
	controlsDock->setFeatures(mainFeatures);
	if (workspaceDock)
		workspaceDock->setFeatures(mainFeatures);
	statsDock->setFeatures(features);

	for (int i = extraDocks.size() - 1; i >= 0; i--) {
		extraDocks[i]->setFeatures(features);
	}

	for (int i = extraCustomDocks.size() - 1; i >= 0; i--) {
		extraCustomDocks[i]->setFeatures(features);
	}

#ifdef BROWSER_AVAILABLE
	for (int i = extraBrowserDocks.size() - 1; i >= 0; i--) {
		extraBrowserDocks[i]->setFeatures(features);
	}
#endif
}

void OBSBasic::on_sideDocks_toggled(bool side)
{
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks", side);

	setDockCornersVertical(side);
}

void OBSBasic::AddDockWidget(QDockWidget *dock, Qt::DockWidgetArea area, bool extraBrowser)
{
	if (dock->objectName().isEmpty()) {
		return;
	}

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	setupDockAction(dock);
	dock->setFeatures(features);
	addDockWidget(area, dock);
	TrackDockForPersistence(dock);

#ifdef BROWSER_AVAILABLE
	if (extraBrowser && extraBrowserMenuDocksSeparator.isNull()) {
		extraBrowserMenuDocksSeparator = ui->menuDocks->addSeparator();
	}

	if (!extraBrowser && !extraBrowserMenuDocksSeparator.isNull()) {
		ui->menuDocks->insertAction(extraBrowserMenuDocksSeparator, dock->toggleViewAction());
	} else {
		ui->menuDocks->addAction(dock->toggleViewAction());
	}

	if (extraBrowser) {
		return;
	}
#else
	UNUSED_PARAMETER(extraBrowser);

	ui->menuDocks->addAction(dock->toggleViewAction());
#endif

	extraDockNames.push_back(dock->objectName());
	extraDocks.push_back(std::shared_ptr<QDockWidget>(dock));
}

void OBSBasic::RemoveDockWidget(const QString &name)
{
	if (extraDockNames.contains(name)) {
		int idx = extraDockNames.indexOf(name);
		extraDockNames.removeAt(idx);
		extraDocks[idx].reset();
		extraDocks.removeAt(idx);
	} else if (extraCustomDockNames.contains(name)) {
		int idx = extraCustomDockNames.indexOf(name);
		extraCustomDockNames.removeAt(idx);
		removeDockWidget(extraCustomDocks[idx]);
		extraCustomDocks.removeAt(idx);
	}
}

bool OBSBasic::IsDockObjectNameUsed(const QString &name)
{
	QStringList list;
	list << "scenesDock"
	     << "sourcesDock"
	     << "mixerDock"
	     << "transitionsDock"
	     << "controlsDock"
	     << "statsDock";
	list << extraDockNames;
	list << extraCustomDockNames;

	return list.contains(name);
}

void OBSBasic::AddCustomDockWidget(QDockWidget *dock)
{
	// Prevent the object name from being changed
	connect(dock, &QObject::objectNameChanged, this, &OBSBasic::RepairCustomExtraDockName);

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	dock->setFeatures(features);
	addDockWidget(Qt::RightDockWidgetArea, dock);
	TrackDockForPersistence(dock);

	extraCustomDockNames.push_back(dock->objectName());
	extraCustomDocks.push_back(dock);
}

void OBSBasic::setDockCornersVertical(bool vertical)
{
	if (vertical) {
		setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
	} else {
		setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
	}
}

void OBSBasic::RepairCustomExtraDockName()
{
	QDockWidget *dock = reinterpret_cast<QDockWidget *>(sender());
	int idx = extraCustomDocks.indexOf(dock);
	QSignalBlocker block(dock);

	if (idx == -1) {
		blog(LOG_WARNING, "A custom dock got its object name changed");
		return;
	}

	blog(LOG_WARNING, "The custom dock '%s' got its object name restored", QT_TO_UTF8(extraCustomDockNames[idx]));

	dock->setObjectName(extraCustomDockNames[idx]);
}

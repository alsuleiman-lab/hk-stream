/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

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

#include <utility/ScreenshotObj.hpp>

#include <qt-wrappers.hpp>

void OBSBasic::Screenshot(OBSSource source)
{
	if (!!screenshotData) {
		blog(LOG_WARNING, "Cannot take new screenshot, "
				  "screenshot currently in progress");
		return;
	}

	screenshotData = new ScreenshotObj(source);

	connect(screenshotData, &ScreenshotObj::imageSaved, this, [this](std::string path) {
		ShowStatusBarMessage(QTStr("Basic.StatusBar.ScreenshotSavedTo").arg(QT_UTF8(path.c_str())));
		lastScreenshot = path;

		OnEvent(OBS_FRONTEND_EVENT_SCREENSHOT_TAKEN);
	});
}

void OBSBasic::ScreenshotSelectedSource()
{
	OBSSceneItem item = GetCurrentSceneItem();
	if (item) {
		Screenshot(obs_sceneitem_get_source(item));
	} else {
		blog(LOG_INFO, "Could not take a source screenshot: "
			       "no source selected");
	}
}

void OBSBasic::ScreenshotProgram()
{
	Screenshot(GetProgramSource());
}

void OBSBasic::ScreenshotScene()
{
	Screenshot(GetCurrentSceneSource());
}

void OBSBasic::TriggerSceneBurstSnapshots(int totalShots, int intervalMs)
{
	OBSSource sceneSource = GetCurrentSceneSource();
	if (!sceneSource) {
		ShowStatusBarMessage(QString::fromUtf8("⚠️ لا يوجد مشهد نشط لالتقاط الصور."));
		return;
	}

	if (sceneBurstSnapshotButton_) {
		sceneBurstSnapshotButton_->setEnabled(false);
	}

	auto timer = new QTimer(this);
	auto currentShot = std::make_shared<int>(0);

	auto performShot = [this, timer, currentShot, totalShots]() {
		OBSSource scene = GetCurrentSceneSource();
		if (!scene) {
			timer->stop();
			timer->deleteLater();
			if (sceneBurstSnapshotButton_) {
				sceneBurstSnapshotButton_->setEnabled(true);
				sceneBurstSnapshotButton_->setText(contextBarSize == ContextBarSize_Normal ? QString::fromUtf8("لقطات (3)") : "");
			}
			return;
		}

		(*currentShot)++;
		int shotNum = *currentShot;

		Screenshot(scene);

		if (sceneBurstSnapshotButton_) {
			sceneBurstSnapshotButton_->setText(QString::fromUtf8("📸 %1/%2").arg(shotNum).arg(totalShots));
		}
		ShowStatusBarMessage(QString::fromUtf8("📸 تم التقاط لقطة المشهد (%1 من %2)...").arg(shotNum).arg(totalShots));

		if (shotNum >= totalShots) {
			timer->stop();
			timer->deleteLater();
			QTimer::singleShot(1000, this, [this, totalShots]() {
				if (sceneBurstSnapshotButton_) {
					sceneBurstSnapshotButton_->setEnabled(true);
					sceneBurstSnapshotButton_->setText(contextBarSize == ContextBarSize_Normal ? QString::fromUtf8("لقطات (3)") : "");
				}
				ShowStatusBarMessage(QString::fromUtf8("✓ تم حفظ %1 لقطات للمشهد بنجاح!").arg(totalShots));
			});
		}
	};

	connect(timer, &QTimer::timeout, this, performShot);

	// First shot immediately
	performShot();

	// Subsequent shots spaced intervalMs (1000ms = 1 second)
	timer->start(intervalMs);
}

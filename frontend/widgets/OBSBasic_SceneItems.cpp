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
#include "ColorSelect.hpp"
#include "OBSProjector.hpp"
#include "workspaces/WorkspaceModel.hpp"
#include "panels/WorkspacePanel.hpp"
#include "dialogs/SourceCategoryDialog.hpp"
#include "dialogs/OBSBasicFilters.hpp"
#include "dialogs/BorderShadowDialog.hpp"
#include "utility/RecycleBin.hpp"

#include <components/VolumeControl.hpp>
#include <dialogs/NameDialog.hpp>
#include <dialogs/OBSBasicAdvAudio.hpp>
#include <dialogs/OBSBasicSourceSelect.hpp>
#include <utility/item-widget-helpers.hpp>

#include <qt-wrappers.hpp>

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>
#include <QWidgetAction>

#include <sstream>
#include <unordered_set>

using namespace std;

namespace {
bool isHiddenInMixer(obs_source_t *source)
{
	OBSDataAutoRelease priv_settings = obs_source_get_private_settings(source);
	bool hidden = obs_data_get_bool(priv_settings, "mixer_hidden");

	return hidden;
}

void setHiddenInMixer(obs_source_t *source, bool hidden)
{
	OBSDataAutoRelease priv_settings = obs_source_get_private_settings(source);
	obs_data_set_bool(priv_settings, "mixer_hidden", hidden);
}

struct SourceRemovalContext {
	obs_source_t *source = nullptr;
	std::vector<OBSSceneItem> items;
	std::unordered_set<obs_sceneitem_t *> seen;
};

bool collectSourceItems(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *context = static_cast<SourceRemovalContext *>(param);
	if (obs_sceneitem_get_source(item) == context->source) {
		if (context->seen.insert(item).second) {
			context->items.emplace_back(item);
		}
	} else if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, collectSourceItems, context);
	}
	return true;
}

bool collectSourceItemsFromScene(void *param, obs_source_t *sceneSource)
{
	auto *context = static_cast<SourceRemovalContext *>(param);
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (scene) {
		obs_scene_enum_items(scene, collectSourceItems, context);
	}
	return true;
}

struct AllScenesSnapshotContext {
	obs_data_array_t *sources = nullptr;
	std::unordered_set<std::string> names;
};

bool addSceneToSnapshot(void *param, obs_source_t *sceneSource)
{
	auto *context = static_cast<AllScenesSnapshotContext *>(param);
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		return true;
	}

	OBSDataAutoRelease snapshot = OBSBasic::BackupScene(scene);
	OBSDataArrayAutoRelease sceneSources = obs_data_get_array(snapshot, "array");
	const size_t count = obs_data_array_count(sceneSources);
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease sourceData = obs_data_array_item(sceneSources, i);
		const char *name = obs_data_get_string(sourceData, "name");
		if (name && context->names.insert(name).second) {
			obs_data_array_push_back(context->sources, sourceData);
		}
	}
	return true;
}

OBSData BackupAllScenes()
{
	OBSDataArrayAutoRelease sources = obs_data_array_create();
	AllScenesSnapshotContext context{sources, {}};
	obs_enum_scenes(addSceneToSnapshot, &context);

	OBSDataAutoRelease snapshot = obs_data_create();
	obs_data_set_array(snapshot, "array", sources);
	obs_data_get_json(snapshot);
	return snapshot.Get();
}

static float sceneItemEffectiveScale(float scale, float boundSize, float sourceSize)
{
	if (sourceSize <= 0.0f || boundSize <= 0.0f || std::abs(scale) <= 0.0001f)
		return scale;
	return std::copysign(boundSize / sourceSize, scale);
}

static void restoreScaleFromSmartFrame(OBSSceneItem item, obs_transform_info &transform)
{
	if (!item || transform.bounds_type == OBS_BOUNDS_NONE)
		return;

	obs_source_t *source = obs_sceneitem_get_source(item);
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_crop(item, &crop);
	const float width = std::max(1.0f, float(obs_source_get_width(source)) - float(crop.left) - float(crop.right));
	const float height = std::max(1.0f, float(obs_source_get_height(source)) - float(crop.top) - float(crop.bottom));
	const float scaleX = std::abs(transform.scale.x);
	const float scaleY = std::abs(transform.scale.y);
	const float contentWidth = width * scaleX;
	const float contentHeight = height * scaleY;
	const float boundsWidth = transform.bounds.x;
	const float boundsHeight = transform.bounds.y;
	float effectiveX = transform.scale.x;
	float effectiveY = transform.scale.y;

	switch (transform.bounds_type) {
	case OBS_BOUNDS_SCALE_INNER:
	case OBS_BOUNDS_SCALE_OUTER:
	case OBS_BOUNDS_MAX_ONLY: {
		const bool exceedsBounds = contentWidth > boundsWidth || contentHeight > boundsHeight;
		if (transform.bounds_type != OBS_BOUNDS_MAX_ONLY || exceedsBounds) {
			const float itemAspect = contentWidth / std::max(1.0f, contentHeight);
			const float boundsAspect = boundsWidth / std::max(1.0f, boundsHeight);
			bool useWidth = boundsAspect < itemAspect;
			if (transform.bounds_type == OBS_BOUNDS_SCALE_OUTER)
				useWidth = !useWidth;
			const float factor = useWidth ? boundsWidth / std::max(1.0f, contentWidth)
					      : boundsHeight / std::max(1.0f, contentHeight);
			effectiveX *= factor;
			effectiveY *= factor;
		}
		break;
	}
	case OBS_BOUNDS_SCALE_TO_WIDTH: {
		const float factor = boundsWidth / std::max(1.0f, contentWidth);
		effectiveX *= factor;
		effectiveY *= factor;
		break;
	}
	case OBS_BOUNDS_SCALE_TO_HEIGHT: {
		const float factor = boundsHeight / std::max(1.0f, contentHeight);
		effectiveX *= factor;
		effectiveY *= factor;
		break;
	}
	case OBS_BOUNDS_STRETCH:
		effectiveX = sceneItemEffectiveScale(transform.scale.x, boundsWidth, width);
		effectiveY = sceneItemEffectiveScale(transform.scale.y, boundsHeight, height);
		break;
	default:
		break;
	}

	vec2_set(&transform.scale, effectiveX, effectiveY);
}

static bool getTextInkBounds(obs_source_t *source, obs_sceneitem_crop &inkCrop)
{
	if (!source || strcmp(obs_source_get_unversioned_id(source), "text_gdiplus") != 0)
		return false;

	proc_handler_t *handler = obs_source_get_proc_handler(source);
	if (!handler)
		return false;

	calldata_t params;
	calldata_init(&params);
	const bool called = proc_handler_call(handler, "get_text_ink_bounds", &params);
	long long left = 0;
	long long top = 0;
	long long width = 0;
	long long height = 0;
	const bool valid = called && calldata_get_int(&params, "left", &left) &&
			    calldata_get_int(&params, "top", &top) &&
			    calldata_get_int(&params, "width", &width) &&
			    calldata_get_int(&params, "height", &height) && width > 0 && height > 0;
	calldata_free(&params);
	if (!valid)
		return false;

	const int sourceWidth = (int)obs_source_get_width(source);
	const int sourceHeight = (int)obs_source_get_height(source);
	if (sourceWidth <= 0 || sourceHeight <= 0 || left < 0 || top < 0 || left + width > sourceWidth ||
	    top + height > sourceHeight)
		return false;

	inkCrop.left = (int)left;
	inkCrop.top = (int)top;
	inkCrop.right = sourceWidth - (int)(left + width);
	inkCrop.bottom = sourceHeight - (int)(top + height);
	return true;
}

static bool getSmartFrameOriginalCrop(OBSSceneItem item, obs_sceneitem_crop &crop)
{
	OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
	if (!settings || !obs_data_get_bool(settings, "hk_smart_frame_text_crop_saved"))
		return false;

	crop.left = (int)obs_data_get_int(settings, "hk_smart_frame_text_crop_left");
	crop.top = (int)obs_data_get_int(settings, "hk_smart_frame_text_crop_top");
	crop.right = (int)obs_data_get_int(settings, "hk_smart_frame_text_crop_right");
	crop.bottom = (int)obs_data_get_int(settings, "hk_smart_frame_text_crop_bottom");
	return true;
}

static void setSmartFrameOriginalCrop(OBSSceneItem item, const obs_sceneitem_crop &crop, bool save)
{
	OBSDataAutoRelease settings = obs_sceneitem_get_private_settings(item);
	if (!settings)
		return;

	if (save) {
		obs_data_set_bool(settings, "hk_smart_frame_text_crop_saved", true);
		obs_data_set_int(settings, "hk_smart_frame_text_crop_left", crop.left);
		obs_data_set_int(settings, "hk_smart_frame_text_crop_top", crop.top);
		obs_data_set_int(settings, "hk_smart_frame_text_crop_right", crop.right);
		obs_data_set_int(settings, "hk_smart_frame_text_crop_bottom", crop.bottom);
	} else {
		obs_data_erase(settings, "hk_smart_frame_text_crop_saved");
		obs_data_erase(settings, "hk_smart_frame_text_crop_left");
		obs_data_erase(settings, "hk_smart_frame_text_crop_top");
		obs_data_erase(settings, "hk_smart_frame_text_crop_right");
		obs_data_erase(settings, "hk_smart_frame_text_crop_bottom");
	}
}

static bool applyTextInkCrop(OBSSceneItem item, obs_source_t *source)
{
	obs_sceneitem_crop inkCrop{};
	if (!getTextInkBounds(source, inkCrop)) {
		blog(LOG_INFO, "[HK Stream] ink crop: no valid ink bounds for '%s' (source %ux%u)",
		     obs_source_get_name(source), obs_source_get_width(source), obs_source_get_height(source));
		return false;
	}

	obs_sceneitem_crop originalCrop{};
	const bool alreadyCropped = getSmartFrameOriginalCrop(item, originalCrop);
	if (!alreadyCropped)
		obs_sceneitem_get_crop(item, &originalCrop);

	const int sourceWidth = (int)obs_source_get_width(source);
	const int sourceHeight = (int)obs_source_get_height(source);
	obs_sceneitem_crop fittedCrop{};
	fittedCrop.left = std::max(originalCrop.left, inkCrop.left);
	fittedCrop.top = std::max(originalCrop.top, inkCrop.top);
	fittedCrop.right = std::max(originalCrop.right, inkCrop.right);
	fittedCrop.bottom = std::max(originalCrop.bottom, inkCrop.bottom);
	if (fittedCrop.left + fittedCrop.right >= sourceWidth ||
	    fittedCrop.top + fittedCrop.bottom >= sourceHeight)
		return false;

	if (!alreadyCropped)
		setSmartFrameOriginalCrop(item, originalCrop, true);
	obs_sceneitem_set_crop(item, &fittedCrop);
	blog(LOG_DEBUG, "[HK Stream] ink crop: '%s' source %dx%d ink crop L%d T%d R%d B%d", obs_source_get_name(source),
	     sourceWidth, sourceHeight, fittedCrop.left, fittedCrop.top, fittedCrop.right, fittedCrop.bottom);
	return true;
}

static bool fitTextSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	/* The visited list stops a scene that (indirectly) contains itself from looping. */
	auto *visited = static_cast<std::vector<obs_scene_t *> *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, fitTextSceneItem, param);
		return true;
	}

	obs_source_t *nested = obs_sceneitem_get_source(item);
	if (nested && obs_source_get_type(nested) == OBS_SOURCE_TYPE_SCENE) {
		/* Composite scenes (for example the YouTube view) show the titles of the
		 * scenes nested inside them, so those titles need fitting too. */
		obs_scene_t *nestedScene = obs_scene_from_source(nested);
		if (nestedScene && visited &&
		    std::find(visited->begin(), visited->end(), nestedScene) == visited->end()) {
			visited->push_back(nestedScene);
			obs_scene_enum_items(nestedScene, fitTextSceneItem, param);
		}
		return true;
	}

	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE)
		return true;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || strcmp(obs_source_get_unversioned_id(source), "text_gdiplus") != 0)
		return true;

	if (applyTextInkCrop(item, source)) {
		obs_transform_info transform{};
		obs_sceneitem_get_info2(item, &transform);
		if (transform.bounds_type == OBS_BOUNDS_NONE)
			transform.bounds_type = OBS_BOUNDS_SCALE_INNER;
		transform.crop_to_bounds = false;
		obs_sceneitem_set_info2(item, &transform);
	}
	return true;
}

std::string getNewSourceName(std::string_view name)
{
	std::string newName{name};
	int suffix = 1;

	for (;;) {
		OBSSourceAutoRelease existing_source = obs_get_source_by_name(newName.c_str());
		if (!existing_source) {
			break;
		}

		char nextName[256];
		std::snprintf(nextName, sizeof(nextName), "%s (%d)", name.data(), ++suffix);
		newName = nextName;
	}

	return newName;
}
} // namespace

static inline bool HasAudioDevices(const char *source_id)
{
	const char *output_id = source_id;
	OBSProperties props = obs_get_source_properties(output_id);
	size_t count = 0;

	if (!props) {
		return false;
	}

	obs_property_t *devices = obs_properties_get(props, "device_id");
	if (devices) {
		count = obs_property_list_item_count(devices);
	}

	return count != 0;
}

void OBSBasic::CreateFirstRunSources()
{
	bool hasDesktopAudio = HasAudioDevices(App()->OutputAudioSource());
	bool hasInputAudio = HasAudioDevices(App()->InputAudioSource());

#ifdef __APPLE__
	/* On macOS 13 and above, the SCK based audio capture provides a
	 * better alternative to the device-based audio capture. */
	if (__builtin_available(macOS 13.0, *)) {
		hasDesktopAudio = false;
	}
#endif

	if (hasDesktopAudio) {
		ResetAudioDevice(App()->OutputAudioSource(), "default", Str("Basic.DesktopDevice1"), 1);
	}
	if (hasInputAudio) {
		ResetAudioDevice(App()->InputAudioSource(), "default", Str("Basic.AuxDevice1"), 3);
	}
}

OBSSceneItem OBSBasic::GetSceneItem(QListWidgetItem *item)
{
	return item ? GetOBSRef<OBSSceneItem>(item) : nullptr;
}

OBSSceneItem OBSBasic::GetCurrentSceneItem()
{
	return ui->sources->Get(GetTopSelectedSourceItem());
}

static void RenameListValues(QListWidget *listWidget, const QString &newName, const QString &prevName)
{
	QList<QListWidgetItem *> items = listWidget->findItems(prevName, Qt::MatchExactly);

	for (int i = 0; i < items.count(); i++) {
		items[i]->setText(newName);
	}
}

void OBSBasic::RenameSources(OBSSource source, QString newName, QString prevName)
{
	RenameListValues(ui->scenes, newName, prevName);

	if (vcamConfig.type == VCamOutputType::SourceOutput && prevName == QString::fromStdString(vcamConfig.source)) {
		vcamConfig.source = newName.toStdString();
	}
	if (vcamConfig.type == VCamOutputType::SceneOutput && prevName == QString::fromStdString(vcamConfig.scene)) {
		vcamConfig.scene = newName.toStdString();
	}

	SaveProject();

	obs_scene_t *scene = obs_scene_from_source(source);
	if (scene) {
		OBSProjector::UpdateMultiviewProjectors();
	}

	UpdateContextBar();
	UpdatePreviewProgramIndicators();
}

bool OBSBasic::QueryRemoveSource(obs_source_t *source)
{
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !obs_source_is_group(source)) {
		int count = ui->scenes->count();

		if (count == 1) {
			OBSMessageBox::information(this, QTStr("FinalScene.Title"), QTStr("FinalScene.Text"));
			return false;
		}
	}

	const char *name = obs_source_get_name(source);

	QString text = QTStr("ConfirmRemove.Text").arg(QT_UTF8(name));

	QMessageBox remove_source(this);
	remove_source.setText(text);
	QPushButton *Yes = remove_source.addButton(QTStr("Yes"), QMessageBox::YesRole);
	remove_source.setDefaultButton(Yes);
	remove_source.addButton(QTStr("No"), QMessageBox::NoRole);
	remove_source.setIcon(QMessageBox::Question);
	remove_source.setWindowTitle(QTStr("ConfirmRemove.Title"));
	remove_source.exec();

	return Yes == remove_source.clickedButton();
}

void OBSBasic::ReorderSources(OBSScene scene)
{
	if (scene != GetCurrentScene() || ui->sources->IgnoreReorder()) {
		return;
	}

	ui->sources->ReorderItems();
	SaveProject();
}

void OBSBasic::RefreshSources(OBSScene scene)
{
	if (scene != GetCurrentScene() || ui->sources->IgnoreReorder()) {
		return;
	}

	ui->sources->RefreshItems();
	SaveProject();
}

void OBSBasic::SourceCreated(void *data, calldata_t *params)
{
	OBSSource source = (obs_source_t *)calldata_ptr(params, "source");

	if (obs_scene_from_source(source) != NULL) {
		QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::AddScene, WaitConnection(), source);
	}
}

void OBSBasic::SourceRemoved(void *data, calldata_t *params)
{
	OBSSource source = (obs_source_t *)calldata_ptr(params, "source");

	if (obs_scene_from_source(source) != NULL) {
		QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::RemoveScene, source);
	}
}

void OBSBasic::SourceRenamed(void *data, calldata_t *params)
{
	OBSSource source = (obs_source_t *)calldata_ptr(params, "source");
	const char *newName = calldata_string(params, "new_name");
	const char *prevName = calldata_string(params, "prev_name");

	QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::RenameSources, source, QT_UTF8(newName),
				  QT_UTF8(prevName));

	blog(LOG_INFO, "Source '%s' renamed to '%s'", prevName, newName);
}

void OBSBasic::ResetAudioDevice(const char *sourceId, const char *deviceId, const char *deviceDesc, int channel)
{
	bool disable = deviceId && strcmp(deviceId, "disabled") == 0;
	OBSSourceAutoRelease source;
	OBSDataAutoRelease settings;

	source = obs_get_output_source(channel);
	if (source) {
		if (disable) {
			obs_set_output_source(channel, nullptr);
		} else {
			settings = obs_source_get_settings(source);
			const char *oldId = obs_data_get_string(settings, "device_id");
			if (strcmp(oldId, deviceId) != 0) {
				obs_data_set_string(settings, "device_id", deviceId);
				obs_source_update(source, settings);
			}
		}

	} else if (!disable) {
		std::string name = getNewSourceName(deviceDesc);

		settings = obs_data_create();
		obs_data_set_string(settings, "device_id", deviceId);
		source = obs_source_create(sourceId, name.c_str(), settings, nullptr);

		obs_set_output_source(channel, source);
	}
}

void OBSBasic::SetDeinterlacingMode()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	obs_deinterlace_mode mode = (obs_deinterlace_mode)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(sceneItem);

	obs_deinterlace_mode oldMode = obs_source_get_deinterlace_mode(source);
	obs_source_set_deinterlace_mode(source, mode);

	auto undo_redo = [](const std::string &uuid, obs_deinterlace_mode val) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (source) {
			obs_source_set_deinterlace_mode(source, val);
		}
	};

	const char *uuid = obs_source_get_uuid(source);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.DeinterlacingMode").arg(obs_source_get_name(source));

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, oldMode);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, mode);

		undo_s.add_action(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

void OBSBasic::SetDeinterlacingOrder()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	obs_deinterlace_field_order order = (obs_deinterlace_field_order)action->property("order").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(sceneItem);

	obs_deinterlace_field_order oldOrder = obs_source_get_deinterlace_field_order(source);
	obs_source_set_deinterlace_field_order(source, order);

	auto undo_redo = [](const std::string &uuid, obs_deinterlace_field_order val) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (source) {
			obs_source_set_deinterlace_field_order(source, val);
		}
	};

	const char *uuid = obs_source_get_uuid(source);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.DeinterlacingOrder").arg(obs_source_get_name(source));

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, oldOrder);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, order);

		undo_s.add_action(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

QMenu *OBSBasic::AddDeinterlacingMenu(QMenu *menu, obs_source_t *source)
{
	obs_deinterlace_mode deinterlaceMode = obs_source_get_deinterlace_mode(source);
	obs_deinterlace_field_order deinterlaceOrder = obs_source_get_deinterlace_field_order(source);
	QAction *action;

#define ADD_MODE(name, mode)                                                             \
	action = menu->addAction(QTStr("" name), this, &OBSBasic::SetDeinterlacingMode); \
	action->setProperty("mode", (int)mode);                                          \
	action->setCheckable(true);                                                      \
	action->setChecked(deinterlaceMode == mode);

	ADD_MODE("Disable", OBS_DEINTERLACE_MODE_DISABLE);
	ADD_MODE("Deinterlacing.Discard", OBS_DEINTERLACE_MODE_DISCARD);
	ADD_MODE("Deinterlacing.Retro", OBS_DEINTERLACE_MODE_RETRO);
	ADD_MODE("Deinterlacing.Blend", OBS_DEINTERLACE_MODE_BLEND);
	ADD_MODE("Deinterlacing.Blend2x", OBS_DEINTERLACE_MODE_BLEND_2X);
	ADD_MODE("Deinterlacing.Linear", OBS_DEINTERLACE_MODE_LINEAR);
	ADD_MODE("Deinterlacing.Linear2x", OBS_DEINTERLACE_MODE_LINEAR_2X);
	ADD_MODE("Deinterlacing.Yadif", OBS_DEINTERLACE_MODE_YADIF);
	ADD_MODE("Deinterlacing.Yadif2x", OBS_DEINTERLACE_MODE_YADIF_2X);
#undef ADD_MODE

	menu->addSeparator();

#define ADD_ORDER(name, order)                                                                          \
	action = menu->addAction(QTStr("Deinterlacing." name), this, &OBSBasic::SetDeinterlacingOrder); \
	action->setProperty("order", (int)order);                                                       \
	action->setCheckable(true);                                                                     \
	action->setChecked(deinterlaceOrder == order);

	ADD_ORDER("TopFieldFirst", OBS_DEINTERLACE_FIELD_ORDER_TOP);
	ADD_ORDER("BottomFieldFirst", OBS_DEINTERLACE_FIELD_ORDER_BOTTOM);
#undef ADD_ORDER

	return menu;
}

void OBSBasic::SetScaleFilter()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	obs_scale_type mode = (obs_scale_type)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();

	obs_scale_type oldMode = obs_sceneitem_get_scale_filter(sceneItem);
	obs_sceneitem_set_scale_filter(sceneItem, mode);

	auto undo_redo = [](const std::string &uuid, int64_t id, obs_scale_type val) {
		OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
		obs_scene_t *sc = obs_group_or_scene_from_source(s);
		obs_sceneitem_t *si = obs_scene_find_sceneitem_by_id(sc, id);
		if (si) {
			obs_sceneitem_set_scale_filter(si, val);
		}
	};

	OBSSource source = obs_sceneitem_get_source(sceneItem);
	OBSSource sceneSource = obs_scene_get_source(obs_sceneitem_get_scene(sceneItem));
	int64_t id = obs_sceneitem_get_id(sceneItem);
	const char *name = obs_source_get_name(sceneSource);
	const char *uuid = obs_source_get_uuid(sceneSource);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.ScaleFiltering").arg(obs_source_get_name(source), name);

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, id, oldMode);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, id, mode);

		undo_s.add_action(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

QMenu *OBSBasic::AddScaleFilteringMenu(QMenu *menu, obs_sceneitem_t *item)
{
	obs_scale_type scaleFilter = obs_sceneitem_get_scale_filter(item);
	QAction *action;

#define ADD_MODE(name, mode)                                                       \
	action = menu->addAction(QTStr("" name), this, &OBSBasic::SetScaleFilter); \
	action->setProperty("mode", (int)mode);                                    \
	action->setCheckable(true);                                                \
	action->setChecked(scaleFilter == mode);

	ADD_MODE("Disable", OBS_SCALE_DISABLE);
	ADD_MODE("ScaleFiltering.Point", OBS_SCALE_POINT);
	ADD_MODE("ScaleFiltering.Bilinear", OBS_SCALE_BILINEAR);
	ADD_MODE("ScaleFiltering.Bicubic", OBS_SCALE_BICUBIC);
	ADD_MODE("ScaleFiltering.Lanczos", OBS_SCALE_LANCZOS);
	ADD_MODE("ScaleFiltering.Area", OBS_SCALE_AREA);
#undef ADD_MODE

	return menu;
}

void OBSBasic::SetBlendingMethod()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	obs_blending_method method = (obs_blending_method)action->property("method").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();

	obs_blending_method oldMethod = obs_sceneitem_get_blending_method(sceneItem);
	obs_sceneitem_set_blending_method(sceneItem, method);

	auto undo_redo = [](const std::string &uuid, int64_t id, obs_blending_method val) {
		OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
		obs_scene_t *sc = obs_group_or_scene_from_source(s);
		obs_sceneitem_t *si = obs_scene_find_sceneitem_by_id(sc, id);
		if (si) {
			obs_sceneitem_set_blending_method(si, val);
		}
	};

	OBSSource source = obs_sceneitem_get_source(sceneItem);
	OBSSource sceneSource = obs_scene_get_source(obs_sceneitem_get_scene(sceneItem));
	int64_t id = obs_sceneitem_get_id(sceneItem);
	const char *name = obs_source_get_name(sceneSource);
	const char *uuid = obs_source_get_uuid(sceneSource);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.BlendingMethod").arg(obs_source_get_name(source), name);

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, id, oldMethod);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, id, method);

		undo_s.add_action(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

QMenu *OBSBasic::AddBlendingMethodMenu(QMenu *menu, obs_sceneitem_t *item)
{
	obs_blending_method blendingMethod = obs_sceneitem_get_blending_method(item);
	QAction *action;

#define ADD_MODE(name, method)                                                        \
	action = menu->addAction(QTStr("" name), this, &OBSBasic::SetBlendingMethod); \
	action->setProperty("method", (int)method);                                   \
	action->setCheckable(true);                                                   \
	action->setChecked(blendingMethod == method);

	ADD_MODE("BlendingMethod.Default", OBS_BLEND_METHOD_DEFAULT);
	ADD_MODE("BlendingMethod.SrgbOff", OBS_BLEND_METHOD_SRGB_OFF);
#undef ADD_MODE

	return menu;
}

void OBSBasic::SetBlendingMode()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	obs_blending_type mode = (obs_blending_type)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();

	obs_blending_type oldMode = obs_sceneitem_get_blending_mode(sceneItem);
	obs_sceneitem_set_blending_mode(sceneItem, mode);

	auto undo_redo = [](const std::string &uuid, int64_t id, obs_blending_type val) {
		OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
		obs_scene_t *sc = obs_group_or_scene_from_source(s);
		obs_sceneitem_t *si = obs_scene_find_sceneitem_by_id(sc, id);
		if (si) {
			obs_sceneitem_set_blending_mode(si, val);
		}
	};

	OBSSource source = obs_sceneitem_get_source(sceneItem);
	OBSSource sceneSource = obs_scene_get_source(obs_sceneitem_get_scene(sceneItem));
	int64_t id = obs_sceneitem_get_id(sceneItem);
	const char *name = obs_source_get_name(sceneSource);
	const char *uuid = obs_source_get_uuid(sceneSource);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.BlendingMode").arg(obs_source_get_name(source), name);

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, id, oldMode);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, id, mode);

		undo_s.add_action(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

QMenu *OBSBasic::AddBlendingModeMenu(QMenu *menu, obs_sceneitem_t *item)
{
	obs_blending_type blendingMode = obs_sceneitem_get_blending_mode(item);
	QAction *action;

#define ADD_MODE(name, mode)                                                        \
	action = menu->addAction(QTStr("" name), this, &OBSBasic::SetBlendingMode); \
	action->setProperty("mode", (int)mode);                                     \
	action->setCheckable(true);                                                 \
	action->setChecked(blendingMode == mode);

	ADD_MODE("BlendingMode.Normal", OBS_BLEND_NORMAL);
	ADD_MODE("BlendingMode.Additive", OBS_BLEND_ADDITIVE);
	ADD_MODE("BlendingMode.Subtract", OBS_BLEND_SUBTRACT);
	ADD_MODE("BlendingMode.Screen", OBS_BLEND_SCREEN);
	ADD_MODE("BlendingMode.Multiply", OBS_BLEND_MULTIPLY);
	ADD_MODE("BlendingMode.Lighten", OBS_BLEND_LIGHTEN);
	ADD_MODE("BlendingMode.Darken", OBS_BLEND_DARKEN);
#undef ADD_MODE

	return menu;
}

QMenu *OBSBasic::AddBackgroundColorMenu(QMenu *menu, QWidgetAction *widgetAction, ColorSelect *select,
					obs_sceneitem_t *item)
{
	QAction *action;

	menu->setStyleSheet(QString("*[bgColor=\"1\"]{background-color:rgba(255,68,68,33%);}"
				    "*[bgColor=\"2\"]{background-color:rgba(255,255,68,33%);}"
				    "*[bgColor=\"3\"]{background-color:rgba(68,255,68,33%);}"
				    "*[bgColor=\"4\"]{background-color:rgba(68,255,255,33%);}"
				    "*[bgColor=\"5\"]{background-color:rgba(68,68,255,33%);}"
				    "*[bgColor=\"6\"]{background-color:rgba(255,68,255,33%);}"
				    "*[bgColor=\"7\"]{background-color:rgba(68,68,68,33%);}"
				    "*[bgColor=\"8\"]{background-color:rgba(255,255,255,33%);}"));

	obs_data_t *privData = obs_sceneitem_get_private_settings(item);
	obs_data_release(privData);

	obs_data_set_default_int(privData, "color-preset", 0);
	int preset = obs_data_get_int(privData, "color-preset");

	action = menu->addAction(QTStr("Clear"), this, &OBSBasic::ColorChange);
	action->setCheckable(true);
	action->setProperty("bgColor", 0);
	action->setChecked(preset == 0);

	action = menu->addAction(QTStr("CustomColor"), this, &OBSBasic::ColorChange);
	action->setCheckable(true);
	action->setProperty("bgColor", 1);
	action->setChecked(preset == 1);

	menu->addSeparator();

	widgetAction->setDefaultWidget(select);

	for (int i = 1; i < 9; i++) {
		stringstream button;
		button << "preset" << i;
		QPushButton *colorButton = select->findChild<QPushButton *>(button.str().c_str());
		if (preset == i + 1) {
			colorButton->setStyleSheet("border: 2px solid black");
		}

		colorButton->setProperty("bgColor", i);
		connect(colorButton, &QPushButton::released, this, &OBSBasic::ColorChange);
	}

	menu->addAction(widgetAction);

	return menu;
}

void OBSBasic::CreateSourcePopupMenu(int idx, bool preview)
{
	QMenu popup(this);
	delete previewProjectorSource;
	delete sourceProjector;
	delete colorMenu;
	delete colorWidgetAction;
	delete colorSelect;
	delete deinterlaceMenu;

	OBSSceneItem sceneItem;
	obs_source_t *source;
	uint32_t flags;
	bool isAsyncVideo = false;
	bool hasAudio = false;
	bool hasVideo = false;

	bool sourceSelected = idx != -1;

	if (sourceSelected) {
		sceneItem = ui->sources->Get(idx);
		source = obs_sceneitem_get_source(sceneItem);
		flags = obs_source_get_output_flags(source);
		isAsyncVideo = (flags & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO;
		hasAudio = (flags & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO;
		hasVideo = (flags & OBS_SOURCE_VIDEO) == OBS_SOURCE_VIDEO;
	}

	// Add new source
	QAction *addSource = popup.addAction(QTStr("AddSource"), this, &OBSBasic::AddSourceDialog);
	popup.addAction(addSource);
	popup.addSeparator();

	if (!preview && !sourceSelected) {
		QAction *addGroup = new QAction(QTStr("Basic.Main.NewGroup"), this);
		connect(addGroup, &QAction::triggered, ui->sources, &SourceTree::AddGroup);
		popup.addAction(addGroup);
	}

	// Preview menu entries
	if (preview) {
		QAction *action =
			popup.addAction(QTStr("Basic.Main.PreviewConextMenu.Enable"), this, &OBSBasic::TogglePreview);
		action->setCheckable(true);
		action->setChecked(obs_display_enabled(ui->preview->GetDisplay()));
		if (IsPreviewProgramMode()) {
			action->setEnabled(false);
		}

		popup.addAction(ui->actionLockPreview);
		popup.addMenu(ui->scalingMenu);

		popup.addSeparator();
	}

	// Projector menu entries
	if (preview) {
		popup.addAction(QT_UTF8("⛶ ملء الشاشة لتحرير العناصر (F11)"), this, &OBSBasic::ToggleFullscreenCanvas);
		popup.addSeparator();

		previewProjectorSource = new QMenu(QTStr("Projector.Open.Preview"));
		AddProjectorMenuMonitors(previewProjectorSource, this, &OBSBasic::OpenPreviewProjector);
		previewProjectorSource->addSeparator();
		previewProjectorSource->addAction(QTStr("Projector.Window"), this, &OBSBasic::OpenPreviewWindow);

		popup.addMenu(previewProjectorSource);
	}

	if (hasVideo) {
		sourceProjector = new QMenu(QTStr("Projector.Open.Source"));
		AddProjectorMenuMonitors(sourceProjector, this, &OBSBasic::OpenSourceProjector);
		sourceProjector->addSeparator();
		sourceProjector->addAction(QTStr("Projector.Window"), this, &OBSBasic::OpenSourceWindow);

		popup.addMenu(sourceProjector);
	}

	popup.addSeparator();

	// Screenshot menu entries
	if (preview) {
		popup.addAction(QTStr("Screenshot.Preview"), this, &OBSBasic::ScreenshotScene);
	}

	if (hasVideo) {
		popup.addAction(QTStr("Screenshot.Source"), this, &OBSBasic::ScreenshotSelectedSource);
	}

	if (sourceSelected) {
		if (workspaceManager_ && source) {
			const OBSWorkspace *ws = workspaceManager_->GetActive();
			if (ws) {
				const char *srcName = obs_source_get_name(source);
				if (srcName && *srcName) {
					std::string nameStr(srcName);
					int cat = workspaceManager_->GetSourceCategory(ws->id, nameStr);

					QMenu *catMenu = popup.addMenu(QString::fromUtf8("🛡️ تصنيف المصدر في بيئة العمل"));

					QAction *actCore = catMenu->addAction(QString::fromUtf8("🛡️ مصدر أساسي (حماية وقفل أثناء البث)"));
					actCore->setCheckable(true);
					actCore->setChecked(cat == 0);
					connect(actCore, &QAction::triggered, this, [this, wsId = ws->id, nameStr]() {
						if (!workspaceManager_->SetSourceCategory(wsId, nameStr, 0)) {
							QMessageBox::warning(this, QString::fromUtf8("تصنيف المصدر"),
									    QString::fromUtf8("تعذر حفظ تصنيف المصدر في ملف بيئة العمل."));
							return;
						}
						RefreshSourceCategoryState();
					});

					QAction *actVar = catMenu->addAction(QString::fromUtf8("⚡ مصدر متغير"));
					actVar->setCheckable(true);
					actVar->setChecked(cat == 1);
					connect(actVar, &QAction::triggered, this, [this, wsId = ws->id, nameStr]() {
						if (!workspaceManager_->SetSourceCategory(wsId, nameStr, 1)) {
							QMessageBox::warning(this, QString::fromUtf8("تصنيف المصدر"),
									    QString::fromUtf8("تعذر حفظ تصنيف المصدر في ملف بيئة العمل."));
							return;
						}
						RefreshSourceCategoryState();
					});

					QAction *actNone = catMenu->addAction(QString::fromUtf8("📋 غير مصنف (إزالة من التصنيف)"));
					actNone->setCheckable(true);
					actNone->setChecked(cat == -1);
					connect(actNone, &QAction::triggered, this, [this, wsId = ws->id, nameStr]() {
						if (!workspaceManager_->SetSourceCategory(wsId, nameStr, -1)) {
							QMessageBox::warning(this, QString::fromUtf8("تصنيف المصدر"),
									    QString::fromUtf8("تعذر حفظ تصنيف المصدر في ملف بيئة العمل."));
							return;
						}
						RefreshSourceCategoryState();
					});

					catMenu->addSeparator();
					QAction *actOpenEditor = catMenu->addAction(QString::fromUtf8("⚙️ فتح محرر مصادر بيئة العمل..."));
					connect(actOpenEditor, &QAction::triggered, this, &OBSBasic::OpenSourceCategoryDialog);

					popup.addSeparator();
				}
			}
		}

		// Sources list menu entries
		if (!preview) {
			colorMenu = new QMenu(QTStr("ChangeBG"));
			colorWidgetAction = new QWidgetAction(colorMenu);
			colorSelect = new ColorSelect(colorMenu);
			popup.addMenu(AddBackgroundColorMenu(colorMenu, colorWidgetAction, colorSelect, sceneItem));

			if (hasAudio) {
				bool isHidden = isHiddenInMixer(source);

				QAction *actionHideMixer =
					popup.addAction(QTStr("HideMixer"), this, [source, isHidden]() {
						setHiddenInMixer(source, !isHidden);

						OBSBasic *main = OBSBasic::Get();
						emit main->mixerStatusChanged(obs_source_get_uuid(source));
					});
				actionHideMixer->setCheckable(true);
				actionHideMixer->setChecked(isHidden);
			}
			popup.addSeparator();
		}

		// Scene item menu entries
		if (hasVideo && source) {
			if (isAsyncVideo) {
				deinterlaceMenu = new QMenu(QTStr("Deinterlacing"));
				popup.addMenu(AddDeinterlacingMenu(deinterlaceMenu, source));
			}

			popup.addSeparator();

			QAction *resizeOutput = popup.addAction(QTStr("ResizeOutputSizeOfSource"), this,
								&OBSBasic::ResizeOutputSizeOfSource);

			int width = obs_source_get_width(source);
			int height = obs_source_get_height(source);

			resizeOutput->setEnabled(!obs_video_active());

			if (width < 32 || height < 32) {
				resizeOutput->setEnabled(false);
			}
		}

		popup.addSeparator();

		popup.addMenu(ui->orderMenu);

		if (hasVideo) {
			obs_bounds_type currentBounds = obs_sceneitem_get_bounds_type(sceneItem);
			bool hasStretchFrame = (currentBounds == OBS_BOUNDS_STRETCH);
			bool hasNoFrame = (currentBounds == OBS_BOUNDS_NONE);

			QMenu *frameMenu = popup.addMenu(QString::fromUtf8("🔲 إطار التناسب الذكي (Smart Frame)"));

			QAction *actFitBounds = frameMenu->addAction(QString::fromUtf8("🔲 ملاءمة كاملة داخل الإطار (الحفاظ على النسبة)"));
			actFitBounds->setCheckable(true);
			actFitBounds->setChecked(currentBounds == OBS_BOUNDS_SCALE_INNER);
			actFitBounds->setToolTip(QString::fromUtf8("يحافظ على أبعاد المصدر ويحتويه داخل حدود الإطار؛ يمكن تغيير عرض الإطار وارتفاعه مستقلين."));
			connect(actFitBounds, &QAction::triggered, this, [this, sceneItem]() {
				ToggleSmartFrame(sceneItem, OBS_BOUNDS_SCALE_INNER);
			});

			QAction *actFitWidth = frameMenu->addAction(QString::fromUtf8("↔️ ملاءمة العرض داخل الإطار"));
			actFitWidth->setCheckable(true);
			actFitWidth->setChecked(currentBounds == OBS_BOUNDS_SCALE_TO_WIDTH);
			actFitWidth->setToolTip(QString::fromUtf8("يضبط حجم المصدر بحسب عرض الإطار مع قص الزيادة عند الحاجة."));
			connect(actFitWidth, &QAction::triggered, this, [this, sceneItem]() {
				ToggleSmartFrame(sceneItem, OBS_BOUNDS_SCALE_TO_WIDTH);
			});

			QAction *actFitHeight = frameMenu->addAction(QString::fromUtf8("↕️ ملاءمة الارتفاع داخل الإطار"));
			actFitHeight->setCheckable(true);
			actFitHeight->setChecked(currentBounds == OBS_BOUNDS_SCALE_TO_HEIGHT);
			actFitHeight->setToolTip(QString::fromUtf8("يضبط حجم المصدر بحسب ارتفاع الإطار مع قص الزيادة عند الحاجة."));
			connect(actFitHeight, &QAction::triggered, this, [this, sceneItem]() {
				ToggleSmartFrame(sceneItem, OBS_BOUNDS_SCALE_TO_HEIGHT);
			});

			QAction *actStretchBounds = frameMenu->addAction(QString::fromUtf8("📐 تمديد لملء الإطار بالكامل (Stretch to Bounds)"));
			actStretchBounds->setCheckable(true);
			actStretchBounds->setChecked(hasStretchFrame);
			connect(actStretchBounds, &QAction::triggered, this, [this, sceneItem]() {
				ToggleSmartFrame(sceneItem, OBS_BOUNDS_STRETCH);
			});

			QAction *actNoBounds = frameMenu->addAction(QString::fromUtf8("⭕ إزالة الإطار (تحجيم حر بدون تقييد)"));
			actNoBounds->setCheckable(true);
			actNoBounds->setChecked(hasNoFrame);
			connect(actNoBounds, &QAction::triggered, this, [this, sceneItem]() {
				ToggleSmartFrame(sceneItem, OBS_BOUNDS_NONE);
			});

			frameMenu->addSeparator();

			QAction *actResetSourceSize = frameMenu->addAction(QString::fromUtf8("🎯 إعادة ضبط حجم الإطار ليطابق حجم النافذة الحالي"));
			connect(actResetSourceSize, &QAction::triggered, this, [this, sceneItem]() {
				ResetSmartFrameToSourceSize(sceneItem);
			});

			QAction *actFitScreen = frameMenu->addAction(QString::fromUtf8("🖥️ تمديد الإطار الذكي لملء الشاشة كاملة"));
			connect(actFitScreen, &QAction::triggered, this, [this, sceneItem]() {
				FitSmartFrameToScreen(sceneItem);
			});

			popup.addMenu(ui->transformMenu);
		}

		popup.addSeparator();

		// Source grouping
		if (ui->sources->MultipleBaseSelected()) {
			popup.addAction(QTStr("Basic.Main.GroupItems"), ui->sources, &SourceTree::GroupSelectedItems);
			popup.addSeparator();
		} else if (ui->sources->GroupsSelected()) {
			popup.addAction(QTStr("Basic.Main.Ungroup"), ui->sources, &SourceTree::UngroupSelectedGroups);
			popup.addSeparator();
		}

		popup.addAction(ui->actionCopySource);
		popup.addAction(ui->actionPasteRef);
		popup.addAction(ui->actionPasteDup);
		popup.addSeparator();

		if (hasVideo || hasAudio) {
			popup.addAction(ui->actionCopyFilters);
			popup.addAction(ui->actionPasteFilters);
			popup.addSeparator();
		}

		popup.addAction(ui->actionRemoveSource);
		popup.addAction(QTStr("Source.DeleteFromAllScenes"), this, [this, source]() {
			SourceRemovalContext context;
			context.source = source;
			obs_enum_scenes(collectSourceItemsFromScene, &context);
			if (context.items.empty()) {
				return;
			}

			const char *sourceName = obs_source_get_name(source);
			QMessageBox confirmation(this);
			confirmation.setText(QTStr("ConfirmRemoveFromAllScenes")
					     .arg(QT_UTF8(sourceName))
					     .arg(QString::number(context.items.size())));
			QPushButton *yes = confirmation.addButton(QTStr("Yes"), QMessageBox::YesRole);
			confirmation.setDefaultButton(yes);
			confirmation.addButton(QTStr("No"), QMessageBox::NoRole);
			confirmation.setIcon(QMessageBox::Question);
			confirmation.setWindowTitle(QTStr("ConfirmRemove.Title"));
			confirmation.exec();
			if (confirmation.clickedButton() != yes) {
				return;
			}

			OBSData undoData = BackupAllScenes();
			for (const OBSSceneItem &item : context.items) {
				obs_source_t *itemSource = obs_sceneitem_get_source(item);
				OBSDataAutoRelease sourceData = obs_save_source(itemSource);
				const char *sourceJson = obs_data_get_json(sourceData);

				obs_transform_info transform;
				obs_sceneitem_crop crop;
				obs_sceneitem_get_info2(item, &transform);
				obs_sceneitem_get_crop(item, &crop);
				OBSDataAutoRelease transformData = obs_data_create();
				obs_data_set_double(transformData, "pos_x", transform.pos.x);
				obs_data_set_double(transformData, "pos_y", transform.pos.y);
				obs_data_set_double(transformData, "scale_x", transform.scale.x);
				obs_data_set_double(transformData, "scale_y", transform.scale.y);
				obs_data_set_double(transformData, "rot", transform.rot);
				obs_data_set_int(transformData, "alignment", transform.alignment);
				obs_data_set_int(transformData, "bounds_type", transform.bounds_type);
				obs_data_set_int(transformData, "bounds_alignment", transform.bounds_alignment);
				obs_data_set_double(transformData, "bounds_x", transform.bounds.x);
				obs_data_set_double(transformData, "bounds_y", transform.bounds.y);
				obs_data_set_bool(transformData, "crop_to_bounds", transform.crop_to_bounds);
				obs_data_set_int(transformData, "crop_left", crop.left);
				obs_data_set_int(transformData, "crop_top", crop.top);
				obs_data_set_int(transformData, "crop_right", crop.right);
				obs_data_set_int(transformData, "crop_bottom", crop.bottom);
				obs_data_set_bool(transformData, "visible", obs_sceneitem_visible(item));

				OBSScene parentScene = obs_sceneitem_get_scene(item);
				obs_source_t *parentSceneSource = parentScene ? obs_scene_get_source(parentScene) : nullptr;
				const char *parentSceneName = parentSceneSource ? obs_source_get_name(parentSceneSource) : "";
				RecycleBin::Instance().AddDeletedSource(
					sourceName ? sourceName : "", parentSceneName ? parentSceneName : "",
					sourceJson ? sourceJson : "", obs_data_get_json(transformData));

				if (obs_sceneitem_t *group = obs_sceneitem_get_group(parentScene, item)) {
					obs_sceneitem_group_remove_item(group, item);
				}
				obs_sceneitem_remove(item);
			}

			OBSData redoData = BackupAllScenes();
			CreateSceneUndoRedoAction(
				QTStr("Undo.DeleteFromAllScenes").arg(QT_UTF8(sourceName)), undoData, redoData);
			ui->sources->RefreshItems();
			SaveProject();
			UpdateContextBar();
		});
		popup.addAction(QTStr("Source.ResetToOriginal"), this, [this, sceneItem]() {
			OBSScene scene = obs_sceneitem_get_scene(sceneItem);
			if (!scene) {
				return;
			}

			obs_video_info videoInfo;
			if (!obs_get_video_info(&videoInfo)) {
				return;
			}

			OBSData undoData = BackupScene(scene);
			obs_transform_info transform;
			obs_sceneitem_get_info2(sceneItem, &transform);
			vec2_set(&transform.pos, float(videoInfo.base_width) * 0.5f,
				 float(videoInfo.base_height) * 0.5f);
			vec2_set(&transform.scale, 1.0f, 1.0f);
			transform.rot = 0.0f;
			transform.alignment = OBS_ALIGN_CENTER;
			transform.bounds_type = OBS_BOUNDS_NONE;
			transform.bounds_alignment = OBS_ALIGN_CENTER;
			vec2_zero(&transform.bounds);
			transform.crop_to_bounds = false;

			obs_sceneitem_crop crop = {};
			obs_sceneitem_set_crop(sceneItem, &crop);
			obs_sceneitem_set_info2(sceneItem, &transform);

			OBSData redoData = BackupScene(scene);
			obs_source_t *sceneSource = obs_scene_get_source(scene);
			CreateSceneUndoRedoAction(
				QTStr("Undo.Transform.Reset").arg(obs_source_get_name(sceneSource)), undoData, redoData);
			SaveProject();
		});
		if (hasVideo && source) {
			popup.addAction(QTStr("Source.Border"), this, [this, sceneItem]() {
				OBSSourceAutoRelease filterSource = obs_sceneitem_get_filter_source(sceneItem);
				BorderShadowDialog dlg(this, filterSource);
				dlg.exec();
			});
		}
		if (source && strcmp(obs_source_get_unversioned_id(source), "image_source") == 0) {
			popup.addAction(QTStr("Source.SwapImage"), this, [this, source]() {
				OBSDataAutoRelease currentSettings = obs_source_get_settings(source);
				const QString currentPath = QString::fromUtf8(
					obs_data_get_string(currentSettings, "file"));
				const QString selectedPath = QFileDialog::getOpenFileName(
					this, QTStr("Source.SwapImageDialog"), currentPath,
					QTStr("Source.ImageFiles"));
				if (selectedPath.isEmpty()) {
					return;
				}

				const QFileInfo selectedFile(selectedPath);
				const QString imageDirectory = QString::fromStdString(
					OBSWorkspaceManager::GetStyleFilePath("assets/images"));
				if (!QDir().mkpath(imageDirectory)) {
					QMessageBox::warning(this, QTStr("Source.SwapImage"),
							     QTStr("Source.ImageCopyFailed"));
					return;
				}

				QString projectImagePath = QDir(imageDirectory).filePath(selectedFile.fileName());
				const QString selectedCanonicalPath = selectedFile.canonicalFilePath();
				QString projectCanonicalPath = QFileInfo(projectImagePath).canonicalFilePath();
				if (projectCanonicalPath.isEmpty()) {
					projectCanonicalPath = QFileInfo(projectImagePath).absoluteFilePath();
				}
#ifdef _WIN32
				constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseInsensitive;
#else
				constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseSensitive;
#endif
				const bool alreadyInProject = !selectedCanonicalPath.isEmpty() &&
					selectedCanonicalPath.compare(projectCanonicalPath, pathCaseSensitivity) == 0;
				if (!alreadyInProject && QFile::exists(projectImagePath)) {
					const QString baseName = selectedFile.completeBaseName();
					const QString extension = selectedFile.suffix();
					int suffix = 1;
					do {
						projectImagePath = QDir(imageDirectory).filePath(
							QStringLiteral("%1_%2.%3").arg(baseName).arg(suffix++).arg(extension));
					} while (QFile::exists(projectImagePath));
				}

				if (!alreadyInProject && !QFile::copy(selectedPath, projectImagePath)) {
					QMessageBox::warning(this, QTStr("Source.SwapImage"),
							     QTStr("Source.ImageCopyFailed"));
					return;
				}

				SourceRemovalContext context;
				context.source = source;
				obs_enum_scenes(collectSourceItemsFromScene, &context);
				std::vector<std::pair<OBSSceneItem, obs_transform_info>> transforms;
				transforms.reserve(context.items.size());
				for (const OBSSceneItem &item : context.items) {
					obs_transform_info transform;
					obs_sceneitem_get_info2(item, &transform);
					transforms.emplace_back(item, transform);
				}

				OBSData undoData = BackupAllScenes();
				const float oldWidth = float(obs_source_get_width(source));
				const float oldHeight = float(obs_source_get_height(source));
				OBSDataAutoRelease settings = obs_source_get_settings(source);
				obs_data_set_string(settings, "file", QT_TO_UTF8(projectImagePath));
				obs_source_update(source, settings);

				const float newWidth = float(obs_source_get_width(source));
				const float newHeight = float(obs_source_get_height(source));
				if (oldWidth > 0.0f && oldHeight > 0.0f && newWidth > 0.0f && newHeight > 0.0f) {
					for (auto &[item, transform] : transforms) {
						if (transform.bounds_type == OBS_BOUNDS_NONE) {
							obs_sceneitem_crop crop;
							obs_sceneitem_get_crop(item, &crop);
							const float oldVisibleWidth =
								oldWidth - float(crop.left) - float(crop.right);
							const float oldVisibleHeight =
								oldHeight - float(crop.top) - float(crop.bottom);
							const float newVisibleWidth =
								newWidth - float(crop.left) - float(crop.right);
							const float newVisibleHeight =
								newHeight - float(crop.top) - float(crop.bottom);
							if (oldVisibleWidth > 0.0f && newVisibleWidth > 0.0f) {
								transform.scale.x *= oldVisibleWidth / newVisibleWidth;
							}
							if (oldVisibleHeight > 0.0f && newVisibleHeight > 0.0f) {
								transform.scale.y *= oldVisibleHeight / newVisibleHeight;
							}
						}
						obs_sceneitem_set_info2(item, &transform);
					}
				}

				OBSData redoData = BackupAllScenes();
				CreateSceneUndoRedoAction(
					QTStr("Undo.Source.SwapImage").arg(obs_source_get_name(source)), undoData, redoData);
				SaveProject();
			});
		}
		popup.addAction(renameSource);
		popup.addSeparator();

		if (flags && flags & OBS_SOURCE_INTERACTION) {
			popup.addAction(QTStr("Interact"), this, &OBSBasic::on_actionInteract_triggered);
		}

		popup.addAction(QTStr("Filters"), this, [&]() { OpenFilters(); });
		QAction *action =
			popup.addAction(QTStr("Properties"), this, &OBSBasic::on_actionSourceProperties_triggered);
		action->setEnabled(obs_source_configurable(source));

		const char *sourceId = source ? obs_source_get_unversioned_id(source) : nullptr;
		if (sourceId && strcmp(sourceId, "browser_source") == 0) {
			QAction *actWebCode = popup.addAction(QString::fromUtf8("🌐 تحرير كود الويب والمعاينة الحية..."));
			connect(actWebCode, &QAction::triggered, this, [this, source]() {
				OpenWebSourceCodeDialog(source);
			});
		}
	} else {
		popup.addAction(ui->actionPasteRef);
		popup.addAction(ui->actionPasteDup);
	}

	popup.exec(QCursor::pos());
}

void OBSBasic::actionOpenSourceFilters()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	if (!action->property("source").isValid()) {
		return;
	}

	obs_source_t *source = action->property("source").value<OBSSource>();

	CreateFiltersWindow(source);
}

void OBSBasic::actionOpenSourceProperties()
{
	QAction *action = reinterpret_cast<QAction *>(sender());
	if (!action->property("source").isValid()) {
		return;
	}

	obs_source_t *source = action->property("source").value<OBSSource>();

	CreatePropertiesWindow(source);
}

void OBSBasic::on_sources_customContextMenuRequested(const QPoint &pos)
{
	if (ui->scenes->count()) {
		QModelIndex idx = ui->sources->indexAt(pos);
		CreateSourcePopupMenu(idx.row(), false);
	}
}

static inline bool should_show_properties(obs_source_t *source, const char *id)
{
	if (!source) {
		return false;
	}
	if (strcmp(id, "group") == 0) {
		return false;
	}
	if (!obs_source_configurable(source)) {
		return false;
	}

	uint32_t caps = obs_source_get_output_flags(source);
	if ((caps & OBS_SOURCE_CAP_DONT_SHOW_PROPERTIES) != 0) {
		return false;
	}

	return true;
}

void OBSBasic::AddSourceDialog()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if (!action) {
		return;
	}

	if (addWindow) {
		addWindow->close();
	}

	addWindow = new OBSBasicSourceSelect(this, undo_s);
	addWindow->show();

	addWindow->setAttribute(Qt::WA_DeleteOnClose, true);
	connect(this, &OBSBasic::sourceUuidDropped, addWindow, &OBSBasicSourceSelect::sourceDropped);
}

void OBSBasic::on_actionAddSource_triggered()
{
	AddSourceDialog();
}

static bool remove_items(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	vector<OBSSceneItem> &items = *static_cast<vector<OBSSceneItem> *>(param);

	if (obs_sceneitem_selected(item)) {
		items.emplace_back(item);
	} else if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, remove_items, &items);
	}
	return true;
};

void OBSBasic::on_actionRemoveSource_triggered()
{
	vector<OBSSceneItem> items;
	OBSScene scene = GetCurrentScene();
	obs_source_t *scene_source = obs_scene_get_source(scene);

	obs_scene_enum_items(scene, remove_items, &items);

	if (!items.size()) {
		return;
	}

	/* ------------------------------------- */
	/* confirm action with user              */

	bool confirmed = false;

	if (items.size() > 1) {
		QString text = QTStr("ConfirmRemove.TextMultiple").arg(QString::number(items.size()));

		QMessageBox remove_items(this);
		remove_items.setText(text);
		QPushButton *Yes = remove_items.addButton(QTStr("Yes"), QMessageBox::YesRole);
		remove_items.setDefaultButton(Yes);
		remove_items.addButton(QTStr("No"), QMessageBox::NoRole);
		remove_items.setIcon(QMessageBox::Question);
		remove_items.setWindowTitle(QTStr("ConfirmRemove.Title"));
		remove_items.exec();

		confirmed = Yes == remove_items.clickedButton();
	} else {
		OBSSceneItem &item = items[0];
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source && QueryRemoveSource(source)) {
			confirmed = true;
		}
	}
	if (!confirmed) {
		return;
	}

	/* ----------------------------------------------- */
	/* save undo data                                  */

	OBSData undo_data = BackupScene(scene_source);

	/* ----------------------------------------------- */
	/* remove items                                    */

	std::string parentSceneName = scene_source ? obs_source_get_name(scene_source) : "";
	for (auto &item : items) {
		obs_source_t *src = obs_sceneitem_get_source(item);
		if (src) {
			const char *srcName = obs_source_get_name(src);
			obs_data_t *srcData = obs_save_source(src);
			const char *srcJson = obs_data_get_json(srcData);

			struct obs_transform_info info;
			struct obs_sceneitem_crop crop;
			obs_sceneitem_get_info2(item, &info);
			obs_sceneitem_get_crop(item, &crop);

			OBSDataAutoRelease transData = obs_data_create();
			obs_data_set_double(transData, "pos_x", info.pos.x);
			obs_data_set_double(transData, "pos_y", info.pos.y);
			obs_data_set_double(transData, "scale_x", info.scale.x);
			obs_data_set_double(transData, "scale_y", info.scale.y);
			obs_data_set_double(transData, "rot", info.rot);
			obs_data_set_int(transData, "alignment", info.alignment);
			obs_data_set_int(transData, "bounds_type", info.bounds_type);
			obs_data_set_int(transData, "bounds_alignment", info.bounds_alignment);
			obs_data_set_double(transData, "bounds_x", info.bounds.x);
			obs_data_set_double(transData, "bounds_y", info.bounds.y);
			obs_data_set_bool(transData, "crop_to_bounds", info.crop_to_bounds);
			obs_data_set_int(transData, "crop_left", crop.left);
			obs_data_set_int(transData, "crop_top", crop.top);
			obs_data_set_int(transData, "crop_right", crop.right);
			obs_data_set_int(transData, "crop_bottom", crop.bottom);
			obs_data_set_bool(transData, "visible", obs_sceneitem_visible(item));

			const char *transJson = obs_data_get_json(transData);

			RecycleBin::Instance().AddDeletedSource(
				srcName ? srcName : "",
				parentSceneName,
				srcJson ? srcJson : "",
				transJson ? transJson : "");

			obs_data_release(srcData);
		}

		obs_sceneitem_t *group = obs_sceneitem_get_group(scene, item);
		if (group) {
			obs_sceneitem_group_remove_item(group, item);
		}

		obs_sceneitem_remove(item);
	}

	/* ----------------------------------------------- */
	/* save redo data                                  */

	OBSData redo_data = BackupScene(scene_source);

	/* ----------------------------------------------- */
	/* add undo/redo action                            */

	QString action_name;
	if (items.size() > 1) {
		action_name = QTStr("Undo.Sources.Multi").arg(QString::number(items.size()));
	} else {
		QString str = QTStr("Undo.Delete");
		action_name = str.arg(obs_source_get_name(obs_sceneitem_get_source(items[0])));
	}

	CreateSceneUndoRedoAction(action_name, undo_data, redo_data);
}

void OBSBasic::on_actionInteract_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source) {
		CreateInteractionWindow(source);
	}
}

void OBSBasic::on_actionSourceProperties_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source) {
		CreatePropertiesWindow(source);
	}
}

void OBSBasic::on_actionSourceUp_triggered()
{
	MoveSceneItem(OBS_ORDER_MOVE_UP, QTStr("Undo.MoveUp"));
}

void OBSBasic::on_actionSourceDown_triggered()
{
	MoveSceneItem(OBS_ORDER_MOVE_DOWN, QTStr("Undo.MoveDown"));
}

void OBSBasic::on_actionMoveUp_triggered()
{
	MoveSceneItem(OBS_ORDER_MOVE_UP, QTStr("Undo.MoveUp"));
}

void OBSBasic::on_actionMoveDown_triggered()
{
	MoveSceneItem(OBS_ORDER_MOVE_DOWN, QTStr("Undo.MoveDown"));
}

void OBSBasic::on_actionMoveToTop_triggered()
{
	MoveSceneItem(OBS_ORDER_MOVE_TOP, QTStr("Undo.MoveToTop"));
}

void OBSBasic::on_actionMoveToBottom_triggered()
{
	MoveSceneItem(OBS_ORDER_MOVE_BOTTOM, QTStr("Undo.MoveToBottom"));
}

void OBSBasic::OpenFilters(OBSSource source)
{
	if (source == nullptr) {
		OBSSceneItem item = GetCurrentSceneItem();
		OBSSourceAutoRelease filterSource = obs_sceneitem_get_filter_source(item);
		source = filterSource;
	}
	CreateFiltersWindow(source);
}

void OBSBasic::OpenProperties(OBSSource source)
{
	if (source == nullptr) {
		OBSSceneItem item = GetCurrentSceneItem();
		source = obs_sceneitem_get_source(item);
	}
	CreatePropertiesWindow(source);
}

void OBSBasic::OpenInteraction(OBSSource source)
{
	if (source == nullptr) {
		OBSSceneItem item = GetCurrentSceneItem();
		source = obs_sceneitem_get_source(item);
	}
	CreateInteractionWindow(source);
}

void OBSBasic::OpenEditTransform(OBSSceneItem item)
{
	if (!item) {
		item = GetCurrentSceneItem();
	}
	if (!item) {
		return;
	}
	CreateEditTransformWindow(item);
}

int OBSBasic::GetTopSelectedSourceItem()
{
	QModelIndexList selectedItems = ui->sources->selectionModel()->selectedIndexes();
	return selectedItems.count() ? selectedItems[0].row() : -1;
}

QModelIndexList OBSBasic::GetAllSelectedSourceItems()
{
	return ui->sources->selectionModel()->selectedIndexes();
}

void OBSBasic::on_actionEditTransform_triggered()
{
	const auto item = GetCurrentSceneItem();
	if (!item) {
		return;
	}
	CreateEditTransformWindow(item);
}

void undo_redo(const std::string &data)
{
	OBSDataAutoRelease dat = obs_data_create_from_json(data.c_str());
	OBSSourceAutoRelease source = obs_get_source_by_uuid(obs_data_get_string(dat, "scene_uuid"));
	OBSBasic::Get()->SetCurrentScene(source.Get(), true);

	obs_scene_load_transform_states(data.c_str());
}

static void GetItemBox(obs_sceneitem_t *item, vec3 &tl, vec3 &br)
{
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);

	auto GetMinPos = [&](float x, float y) {
		vec3 pos;
		vec3_set(&pos, x, y, 0.0f);
		vec3_transform(&pos, &pos, &boxTransform);
		vec3_min(&tl, &tl, &pos);
		vec3_max(&br, &br, &pos);
	};

	GetMinPos(0.0f, 0.0f);
	GetMinPos(1.0f, 0.0f);
	GetMinPos(0.0f, 1.0f);
	GetMinPos(1.0f, 1.0f);
}

static vec3 GetItemTL(obs_sceneitem_t *item)
{
	vec3 tl, br;
	GetItemBox(item, tl, br);
	return tl;
}

static void SetItemTL(obs_sceneitem_t *item, const vec3 &tl)
{
	vec3 newTL;
	vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	newTL = GetItemTL(item);
	pos.x += tl.x - newTL.x;
	pos.y += tl.y - newTL.y;
	obs_sceneitem_set_pos(item, &pos);
}

static bool RotateSelectedSources(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, RotateSelectedSources, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	float rot = *static_cast<float *>(param);

	vec3 tl = GetItemTL(item);

	rot += obs_sceneitem_get_rot(item);
	if (rot >= 360.0f) {
		rot -= 360.0f;
	} else if (rot <= -360.0f) {
		rot += 360.0f;
	}
	obs_sceneitem_set_rot(item, rot);

	obs_sceneitem_force_update_transform(item);

	SetItemTL(item, tl);

	return true;
};

void OBSBasic::on_actionRotate90CW_triggered()
{
	float f90CW = 90.0f;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f90CW);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.Rotate").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::on_actionRotate90CCW_triggered()
{
	float f90CCW = -90.0f;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f90CCW);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.Rotate").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::on_actionRotate180_triggered()
{
	float f180 = 180.0f;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	obs_scene_enum_items(GetCurrentScene(), RotateSelectedSources, &f180);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.Rotate").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

static bool MultiplySelectedItemScale(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	vec2 &mul = *static_cast<vec2 *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, MultiplySelectedItemScale, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	vec3 tl = GetItemTL(item);

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	vec2_mul(&scale, &scale, &mul);
	obs_sceneitem_set_scale(item, &scale);

	obs_sceneitem_force_update_transform(item);

	SetItemTL(item, tl);

	return true;
}

void OBSBasic::on_actionFlipHorizontal_triggered()
{
	vec2 scale;
	vec2_set(&scale, -1.0f, 1.0f);
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	obs_scene_enum_items(GetCurrentScene(), MultiplySelectedItemScale, &scale);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.HFlip").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::on_actionFlipVertical_triggered()
{
	vec2 scale;
	vec2_set(&scale, 1.0f, -1.0f);
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	obs_scene_enum_items(GetCurrentScene(), MultiplySelectedItemScale, &scale);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.VFlip").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

static bool CenterAlignSelectedItems(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	obs_bounds_type boundsType = *static_cast<obs_bounds_type *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, CenterAlignSelectedItems, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	obs_transform_info itemInfo;
	vec2_set(&itemInfo.pos, 0.0f, 0.0f);
	vec2_set(&itemInfo.scale, 1.0f, 1.0f);
	itemInfo.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	itemInfo.rot = 0.0f;

	vec2_set(&itemInfo.bounds, float(ovi.base_width), float(ovi.base_height));
	itemInfo.bounds_type = boundsType;
	itemInfo.bounds_alignment = OBS_ALIGN_CENTER;
	itemInfo.crop_to_bounds = obs_sceneitem_get_bounds_crop(item);

	obs_sceneitem_set_info2(item, &itemInfo);

	return true;
}

void OBSBasic::on_actionFitToScreen_triggered()
{
	obs_bounds_type boundsType = OBS_BOUNDS_SCALE_INNER;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	obs_scene_enum_items(GetCurrentScene(), CenterAlignSelectedItems, &boundsType);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.FitToScreen").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::on_actionStretchToScreen_triggered()
{
	obs_bounds_type boundsType = OBS_BOUNDS_STRETCH;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	obs_scene_enum_items(GetCurrentScene(), CenterAlignSelectedItems, &boundsType);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(QTStr("Undo.Transform.StretchToScreen")
				  .arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
			  undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::ToggleSmartFrame(OBSSceneItem item, obs_bounds_type targetType)
{
	if (!item) {
		item = GetCurrentSceneItem();
	}
	if (!item) {
		return;
	}

	obs_bounds_type curType = obs_sceneitem_get_bounds_type(item);

	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	obs_source_t *source = obs_sceneitem_get_source(item);
	int w = (int)obs_source_get_width(source);
	int h = (int)obs_source_get_height(source);
	if (w <= 0) w = 1920;
	if (h <= 0) h = 1080;

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	obs_transform_info oti;
	obs_sceneitem_get_info2(item, &oti);
	const bool isTextSource = strcmp(obs_source_get_unversioned_id(source), "text_gdiplus") == 0;
	if (curType == OBS_BOUNDS_NONE &&
	    (std::abs(oti.scale.x) > 20.0f || std::abs(oti.scale.y) > 20.0f)) {
		// Repair the pixel-dimension scale written by older Smart Frame toggles.
		const float repairedX = oti.scale.x / std::max(1.0f, float(w - crop.left - crop.right));
		const float repairedY = oti.scale.y / std::max(1.0f, float(h - crop.top - crop.bottom));
		if (std::abs(repairedX) <= 10.0f && std::abs(repairedY) <= 10.0f &&
		    std::abs(repairedX) > 0.0001f && std::abs(repairedY) > 0.0001f) {
			vec2_set(&oti.scale, repairedX, repairedY);
		}
	}

	const bool isSmartFit = targetType == OBS_BOUNDS_SCALE_INNER || targetType == OBS_BOUNDS_SCALE_TO_WIDTH ||
			 targetType == OBS_BOUNDS_SCALE_TO_HEIGHT;
	if (targetType == OBS_BOUNDS_NONE ||
	    (curType == targetType && isSmartFit && !isTextSource)) {
		obs_sceneitem_crop originalTextCrop{};
		if (getSmartFrameOriginalCrop(item, originalTextCrop)) {
			obs_sceneitem_set_crop(item, &originalTextCrop);
			setSmartFrameOriginalCrop(item, originalTextCrop, false);
		}

		// Convert the displayed bounds back into free scale before clearing them.
		restoreScaleFromSmartFrame(item, oti);
		oti.bounds_type = OBS_BOUNDS_NONE;
		vec2_zero(&oti.bounds);
		oti.crop_to_bounds = false;
		obs_sceneitem_set_info2(item, &oti);
	} else {
		if (isTextSource && applyTextInkCrop(item, source))
			obs_sceneitem_get_crop(item, &crop);

		// Enable Smart Frame using the visible title glyphs as the text item's edges.
		float currentW = (float(w) - float(crop.left + crop.right)) * std::abs(oti.scale.x);
		float currentH = (float(h) - float(crop.top + crop.bottom)) * std::abs(oti.scale.y);

		if (oti.bounds.x > 10.0f && oti.bounds.y > 10.0f && curType != OBS_BOUNDS_NONE) {
			// keep existing bounds size
		} else {
			if (currentW < 20.0f) currentW = float(w);
			if (currentH < 20.0f) currentH = float(h);
			vec2_set(&oti.bounds, currentW, currentH);
		}

		oti.bounds_type = targetType;
		oti.bounds_alignment = OBS_ALIGN_CENTER;
		oti.crop_to_bounds = !isTextSource &&
				     (targetType == OBS_BOUNDS_SCALE_TO_WIDTH || targetType == OBS_BOUNDS_SCALE_TO_HEIGHT);
		obs_sceneitem_set_info2(item, &oti);
	}

	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.FitToScreen").arg(obs_source_get_name(source)),
		undo_redo, undo_redo, undo_data, redo_data);

	UpdateContextBar();
}

void OBSBasic::FitTextItemsToSmartFrame()
{
	auto fit = [this]() {
		OBSScene scene = GetCurrentScene();
		if (scene) {
			std::vector<obs_scene_t *> visited{scene};
			obs_scene_enum_items(scene, fitTextSceneItem, &visited);
		}
	};
	fit();

	/* obs_source_update() on a video source is deferred to the source's next
	 * video tick, so the ink bounds read above still describe the previous
	 * text. Fit again once the new text has been rendered, otherwise the
	 * title keeps the margins of the title that was shown before it. */
	QTimer::singleShot(150, this, fit);
	QTimer::singleShot(600, this, fit);
}

void OBSBasic::ResetSmartFrameToSourceSize(OBSSceneItem item)
{
	if (!item) {
		item = GetCurrentSceneItem();
	}
	if (!item) return;

	obs_source_t *source = obs_sceneitem_get_source(item);
	int w = (int)obs_source_get_width(source);
	int h = (int)obs_source_get_height(source);
	if (w <= 0 || h <= 0) return;

	const bool isText = strcmp(obs_source_get_unversioned_id(source), "text_gdiplus") == 0;
	if (isText) {
		applyTextInkCrop(item, source);
	} else {
		obs_sceneitem_crop originalTextCrop{};
		if (getSmartFrameOriginalCrop(item, originalTextCrop)) {
			obs_sceneitem_set_crop(item, &originalTextCrop);
			setSmartFrameOriginalCrop(item, originalTextCrop, false);
		}
	}

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	float targetW = std::max(20.0f, float(w - crop.left - crop.right));
	float targetH = std::max(20.0f, float(h - crop.top - crop.bottom));

	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	obs_transform_info oti;
	obs_sceneitem_get_info2(item, &oti);
	vec2_set(&oti.bounds, targetW, targetH);
	oti.bounds_type = OBS_BOUNDS_SCALE_INNER;
	oti.bounds_alignment = OBS_ALIGN_CENTER;
	oti.crop_to_bounds = false;
	vec2_set(&oti.scale, 1.0f, 1.0f);
	obs_sceneitem_set_info2(item, &oti);

	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.FitToScreen").arg(obs_source_get_name(source)),
		undo_redo, undo_redo, undo_data, redo_data);

	UpdateContextBar();
}

void OBSBasic::FitSmartFrameToScreen(OBSSceneItem item)
{
	if (!item) {
		item = GetCurrentSceneItem();
	}
	if (!item) return;

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	obs_transform_info oti;
	obs_sceneitem_get_info2(item, &oti);
	vec2_set(&oti.pos, 0.0f, 0.0f);
	vec2_set(&oti.scale, 1.0f, 1.0f);
	oti.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	vec2_set(&oti.bounds, float(ovi.base_width), float(ovi.base_height));
	oti.bounds_type = OBS_BOUNDS_SCALE_INNER;
	oti.bounds_alignment = OBS_ALIGN_CENTER;
	oti.crop_to_bounds = false;
	obs_sceneitem_set_info2(item, &oti);

	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.FitToScreen").arg(obs_source_get_name(obs_sceneitem_get_source(item))),
		undo_redo, undo_redo, undo_data, redo_data);

	UpdateContextBar();
}

void OBSBasic::CenterSelectedSceneItems(const CenterType &centerType)
{
	QModelIndexList selectedItems = GetAllSelectedSourceItems();

	if (!selectedItems.count()) {
		return;
	}

	vector<OBSSceneItem> items;

	// Filter out items that have no size
	for (int x = 0; x < selectedItems.count(); x++) {
		OBSSceneItem item = ui->sources->Get(selectedItems[x].row());
		obs_transform_info oti;
		obs_sceneitem_get_info2(item, &oti);

		obs_source_t *source = obs_sceneitem_get_source(item);
		float width = float(obs_source_get_width(source)) * oti.scale.x;
		float height = float(obs_source_get_height(source)) * oti.scale.y;

		if (width == 0.0f || height == 0.0f) {
			continue;
		}

		items.emplace_back(item);
	}

	if (!items.size()) {
		return;
	}

	// Get center x, y coordinates of items
	vec3 center;

	float top = M_INFINITE;
	float left = M_INFINITE;
	float right = 0.0f;
	float bottom = 0.0f;

	for (auto &item : items) {
		vec3 tl, br;

		GetItemBox(item, tl, br);

		left = std::min(tl.x, left);
		top = std::min(tl.y, top);
		right = std::max(br.x, right);
		bottom = std::max(br.y, bottom);
	}

	center.x = (right + left) / 2.0f;
	center.y = (top + bottom) / 2.0f;
	center.z = 0.0f;

	// Get coordinates of screen center
	obs_video_info ovi;
	obs_get_video_info(&ovi);

	vec3 screenCenter;
	vec3_set(&screenCenter, float(ovi.base_width), float(ovi.base_height), 0.0f);

	vec3_mulf(&screenCenter, &screenCenter, 0.5f);

	// Calculate difference between screen center and item center
	vec3 offset;
	vec3_sub(&offset, &screenCenter, &center);

	// Shift items by offset
	for (auto &item : items) {
		vec3 tl, br;

		GetItemBox(item, tl, br);

		vec3_add(&tl, &tl, &offset);

		vec3 itemTL = GetItemTL(item);

		if (centerType == CenterType::Vertical) {
			tl.x = itemTL.x;
		} else if (centerType == CenterType::Horizontal) {
			tl.y = itemTL.y;
		}

		SetItemTL(item, tl);
	}
}

void OBSBasic::on_actionCenterToScreen_triggered()
{
	CenterType centerType = CenterType::Scene;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	CenterSelectedSceneItems(centerType);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.Center").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::on_actionVerticalCenter_triggered()
{
	CenterType centerType = CenterType::Vertical;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	CenterSelectedSceneItems(centerType);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.VCenter").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::on_actionHorizontalCenter_triggered()
{
	CenterType centerType = CenterType::Horizontal;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(GetCurrentScene(), false);
	CenterSelectedSceneItems(centerType);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), false);

	std::string undo_data(obs_data_get_json(wrapper));
	std::string redo_data(obs_data_get_json(rwrapper));
	undo_s.add_action(
		QTStr("Undo.Transform.HCenter").arg(obs_source_get_name(obs_scene_get_source(GetCurrentScene()))),
		undo_redo, undo_redo, undo_data, redo_data);
}

void OBSBasic::on_toggleSourceIcons_toggled(bool visible)
{
	ui->sources->SetIconsVisible(visible);
	if (advAudioWindow != nullptr) {
		advAudioWindow->SetIconsVisible(visible);
	}

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "ShowSourceIcons", visible);
}

void OBSBasic::on_sourcePropertiesButton_clicked()
{
	on_actionSourceProperties_triggered();
}

void OBSBasic::on_sourceFiltersButton_clicked()
{
	OpenFilters();
}

void OBSBasic::on_sourceInteractButton_clicked()
{
	on_actionInteract_triggered();
}

#include "RecycleBin.hpp"
#include "workspaces/WorkspaceModel.hpp"
#include <obs.h>
#include <obs-frontend-api.h>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <util/base.h>
#include <algorithm>

#include "moc_RecycleBin.cpp"

static const int64_t kRetentionSeconds = 24 * 3600; // 24 hours

RecycleBin &RecycleBin::Instance()
{
	static RecycleBin s_instance;
	return s_instance;
}

RecycleBin::RecycleBin()
{
	Load();
}

std::string RecycleBin::GetFilePath() const
{
	return OBSWorkspaceManager::GetWorkspaceFilePath("recycle_bin.json");
}

void RecycleBin::PurgeExpired()
{
	int64_t now = QDateTime::currentSecsSinceEpoch();
	size_t beforeCount = items_.size();

	items_.erase(std::remove_if(items_.begin(), items_.end(), [now](const DeletedItem &it) {
		return (now - it.deletedAt) > kRetentionSeconds;
	}), items_.end());

	if (items_.size() != beforeCount) {
		Save();
	}
}

void RecycleBin::Load()
{
	items_.clear();
	QString path = QString::fromStdString(GetFilePath());
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		return;
	}

	QByteArray data = f.readAll();
	f.close();

	if (data.startsWith("\xEF\xBB\xBF")) data.remove(0, 3);

	QJsonDocument doc = QJsonDocument::fromJson(data);
	if (!doc.isObject()) return;

	QJsonArray arr = doc.object()["deleted_items"].toArray();
	for (const auto &val : arr) {
		if (!val.isObject()) continue;
		QJsonObject o = val.toObject();

		DeletedItem it;
		it.id          = o["id"].toString().toStdString();
		it.type        = o["type"].toString().toStdString();
		it.name        = o["name"].toString().toStdString();
		it.parentScene = o["parent_scene"].toString().toStdString();
		it.workspaceId = o["workspace_id"].toString().toStdString();
		it.deletedAt   = o["deleted_at"].toVariant().toLongLong();
		it.jsonData    = o["json_data"].toString().toStdString();
		it.extraJson   = o["extra_json"].toString().toStdString();

		if (!it.id.empty() && !it.name.empty()) {
			items_.push_back(std::move(it));
		}
	}

	PurgeExpired();
}

void RecycleBin::Save()
{
	QString path = QString::fromStdString(GetFilePath());
	QFileInfo fi(path);
	QDir().mkpath(fi.dir().absolutePath());

	QJsonObject root;
	QJsonArray arr;
	for (const auto &it : items_) {
		QJsonObject o;
		o["id"]           = QString::fromStdString(it.id);
		o["type"]         = QString::fromStdString(it.type);
		o["name"]         = QString::fromStdString(it.name);
		o["parent_scene"] = QString::fromStdString(it.parentScene);
		o["workspace_id"] = QString::fromStdString(it.workspaceId);
		o["deleted_at"]   = static_cast<qint64>(it.deletedAt);
		o["json_data"]    = QString::fromStdString(it.jsonData);
		o["extra_json"]   = QString::fromStdString(it.extraJson);
		arr.append(o);
	}
	root["deleted_items"] = arr;

	QJsonDocument doc(root);
	QFile f(path);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(doc.toJson(QJsonDocument::Indented));
		f.close();
	}
}

void RecycleBin::AddDeletedScene(const std::string &sceneName,
				const std::string &wsId,
				const std::string &jsonData,
				const std::string &extraJson)
{
	if (sceneName.empty()) return;

	PurgeExpired();

	int64_t now = QDateTime::currentSecsSinceEpoch();
	for (const auto &existing : items_) {
		if (existing.type == "scene" && existing.name == sceneName && (now - existing.deletedAt) < 3) {
			return;
		}
	}

	DeletedItem it;
	it.id          = "scene_" + std::to_string(now) + "_" + sceneName;
	it.type        = "scene";
	it.name        = sceneName;
	it.workspaceId = wsId;
	it.deletedAt   = now;
	it.jsonData    = jsonData;
	it.extraJson   = extraJson;

	items_.insert(items_.begin(), it);
	Save();
	emit ItemsChanged();
	blog(LOG_INFO, "[RecycleBin] Added deleted scene '%s' (retained for 24h)", sceneName.c_str());
}

void RecycleBin::AddDeletedSource(const std::string &sourceName,
				 const std::string &parentScene,
				 const std::string &jsonData,
				 const std::string &extraJson)
{
	if (sourceName.empty()) return;

	PurgeExpired();

	int64_t now = QDateTime::currentSecsSinceEpoch();
	for (const auto &existing : items_) {
		if (existing.type == "source" && existing.name == sourceName && existing.parentScene == parentScene && (now - existing.deletedAt) < 3) {
			return;
		}
	}

	DeletedItem it;
	it.id          = "source_" + std::to_string(now) + "_" + sourceName;
	it.type        = "source";
	it.name        = sourceName;
	it.parentScene = parentScene;
	it.deletedAt   = now;
	it.jsonData    = jsonData;
	it.extraJson   = extraJson;

	items_.insert(items_.begin(), it);
	Save();
	emit ItemsChanged();
	blog(LOG_INFO, "[RecycleBin] Added deleted source '%s' from scene '%s' (retained for 24h)",
	     sourceName.c_str(), parentScene.c_str());
}

std::vector<DeletedItem> RecycleBin::GetItems()
{
	PurgeExpired();
	return items_;
}

int RecycleBin::GetCount()
{
	PurgeExpired();
	return static_cast<int>(items_.size());
}

bool RecycleBin::PermanentlyDeleteItem(const std::string &id)
{
	auto it = std::find_if(items_.begin(), items_.end(), [&id](const DeletedItem &i) {
		return i.id == id;
	});
	if (it != items_.end()) {
		items_.erase(it);
		Save();
		emit ItemsChanged();
		return true;
	}
	return false;
}

void RecycleBin::ClearAll()
{
	items_.clear();
	Save();
	emit ItemsChanged();
}

bool RecycleBin::RestoreItem(const std::string &id, QString &errorMsg)
{
	auto it = std::find_if(items_.begin(), items_.end(), [&id](const DeletedItem &i) {
		return i.id == id;
	});
	if (it == items_.end()) {
		errorMsg = "العنصر غير موجود في سلة المحذوفات.";
		return false;
	}

	DeletedItem item = *it;

	if (item.type == "scene") {
		// Restore Scene
		obs_source_t *existing = obs_get_source_by_name(item.name.c_str());
		if (existing) {
			obs_source_release(existing);
			errorMsg = QString("يوجد مشهد آخر بنفس الاسم '%1' حالياً.").arg(QString::fromStdString(item.name));
			return false;
		}

		obs_source_t *sceneSrc = nullptr;
		if (!item.jsonData.empty()) {
			obs_data_t *d = obs_data_create_from_json(item.jsonData.c_str());
			if (d) {
				sceneSrc = obs_load_source(d);
				obs_data_release(d);
			}
		}

		if (!sceneSrc) {
			obs_scene_t *sc = obs_scene_create(item.name.c_str());
			if (sc) {
				sceneSrc = obs_scene_get_source(sc);
			}
		}

		if (!sceneSrc) {
			errorMsg = "فشلت استعادة المشهد من البيانات المحفوظة.";
			return false;
		}

		// Add scene back to workspace model
		OBSWorkspaceManager mgr;
		if (mgr.Load()) {
			std::string wsId = item.workspaceId;
			if (wsId.empty() && mgr.GetActive()) {
				wsId = mgr.GetActive()->id;
			}
			OBSWorkspace *ws = mgr.GetById(wsId);
			if (ws) {
				if (std::find(ws->scenes.begin(), ws->scenes.end(), item.name) == ws->scenes.end()) {
					ws->scenes.push_back(item.name);
					mgr.Save();
				}
			}
		}

		obs_frontend_set_current_scene(sceneSrc);
		obs_source_release(sceneSrc);

		items_.erase(it);
		Save();
		emit ItemsChanged();
		blog(LOG_INFO, "[RecycleBin] Successfully restored scene '%s'", item.name.c_str());
		return true;

	} else if (item.type == "source") {
		// Restore Source
		obs_source_t *src = nullptr;
		if (!item.jsonData.empty()) {
			obs_data_t *d = obs_data_create_from_json(item.jsonData.c_str());
			if (d) {
				src = obs_load_source(d);
				obs_data_release(d);
			}
		}

		if (!src) {
			src = obs_get_source_by_name(item.name.c_str());
		}

		if (!src) {
			errorMsg = "فشلت استعادة المصدر من البيانات المحفوظة.";
			return false;
		}

		// Find target scene
		obs_source_t *targetSceneSrc = obs_get_source_by_name(item.parentScene.c_str());
		if (!targetSceneSrc) {
			targetSceneSrc = obs_frontend_get_current_scene();
		}

		if (!targetSceneSrc) {
			obs_source_release(src);
			errorMsg = "لم يتم العثور على المشهد المستهدف لإضافة المصدر إليه.";
			return false;
		}

		obs_scene_t *scene = obs_scene_from_source(targetSceneSrc);
		if (!scene) {
			obs_source_release(targetSceneSrc);
			obs_source_release(src);
			errorMsg = "المشهد المستهدف ليس مشهداً صالحاً.";
			return false;
		}

		obs_sceneitem_t *newItem = obs_scene_add(scene, src);
		if (newItem && !item.extraJson.empty()) {
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(item.extraJson));
			if (doc.isObject()) {
				QJsonObject o = doc.object();
				struct obs_transform_info info;
				obs_sceneitem_get_info2(newItem, &info);
				if (o.contains("pos_x")) info.pos.x = (float)o["pos_x"].toDouble();
				if (o.contains("pos_y")) info.pos.y = (float)o["pos_y"].toDouble();
				if (o.contains("scale_x")) info.scale.x = (float)o["scale_x"].toDouble();
				if (o.contains("scale_y")) info.scale.y = (float)o["scale_y"].toDouble();
				obs_sceneitem_set_info2(newItem, &info);
			}
		}

		obs_source_release(targetSceneSrc);
		obs_source_release(src);

		items_.erase(it);
		Save();
		emit ItemsChanged();
		blog(LOG_INFO, "[RecycleBin] Successfully restored source '%s' to scene '%s'",
		     item.name.c_str(), item.parentScene.c_str());
		return true;
	}

	errorMsg = "نوع العنصر غير معروف.";
	return false;
}

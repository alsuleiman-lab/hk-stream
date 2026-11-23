#include "WorkspaceModel.hpp"
#include "HKProjectStore.hpp"
#include <obs.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <jansson.h>
#include <fstream>
#include <sstream>
#include <util/base.h>
#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QColor>
#include <algorithm>
#include <cctype>
#include <utility>

extern bool portable_mode;

namespace {
std::string ResolutionKey(uint32_t cx, uint32_t cy)
{
	return std::to_string(cx) + "x" + std::to_string(cy);
}

void MigrateWorkspaceDirectory(const QString &legacyPath, const QString &persistentPath)
{
	if (!QDir(legacyPath).exists() || QFileInfo(legacyPath).canonicalFilePath() ==
					  QFileInfo(persistentPath).canonicalFilePath()) {
		return;
	}

	QDir sourceDir(legacyPath);
	QDirIterator iterator(legacyPath, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		const QString sourceFile = iterator.next();
		const QString relativePath = sourceDir.relativeFilePath(sourceFile);
		const QString targetFile = QDir(persistentPath).filePath(relativePath);
		if (QFileInfo::exists(targetFile)) {
			continue;
		}

		if (!QDir().mkpath(QFileInfo(targetFile).absolutePath()) || !QFile::copy(sourceFile, targetFile)) {
			blog(LOG_WARNING, "[Workspaces] Could not migrate %s to persistent storage: %s",
			     qPrintable(sourceFile), qPrintable(targetFile));
		}
	}
}

void BackupValidWorkspaceFile(const QString &path)
{
	QFile current(path);
	if (!current.exists() || !current.open(QIODevice::ReadOnly)) {
		return;
	}

	const QByteArray data = current.readAll();
	if (current.error() != QFileDevice::NoError) {
		return;
	}

	json_error_t error{};
	json_t *root = json_loadb(data.constData(), static_cast<size_t>(data.size()), 0, &error);
	if (!root) {
		blog(LOG_WARNING, "[Workspaces] Keeping the previous backup because %s is not valid JSON",
		     qPrintable(path));
		return;
	}

	const bool validWorkspace = json_is_object(root) &&
				    json_is_string(json_object_get(root, "active_workspace")) &&
				    json_is_array(json_object_get(root, "workspaces"));
	json_decref(root);
	if (!validWorkspace) {
		blog(LOG_WARNING, "[Workspaces] Keeping the previous backup because %s has no workspace data",
		     qPrintable(path));
		return;
	}

	QSaveFile backup(path + ".bak");
	if (!backup.open(QIODevice::WriteOnly) || backup.write(data) != data.size() || !backup.commit()) {
		blog(LOG_WARNING, "[Workspaces] Could not update backup %s: %s", qPrintable(backup.fileName()),
		     qPrintable(backup.errorString()));
	}
}
}

// -------------------------------------------------------
// Path Helpers
// -------------------------------------------------------
std::string OBSWorkspaceManager::GetWorkspacesDirPath()
{
    QString appDir = QCoreApplication::applicationDirPath();
    // Explicit portable mode keeps data alongside the portable installation.
    QDir cand1(appDir + "/../../workspaces");
    if (portable_mode) {
        if (!cand1.exists())
            cand1.mkpath(".");
        return cand1.canonicalPath().toStdString();
    }

    // Normal installs must keep user data outside rundir/build directories.
    char confPath[512] = {0};
    if (os_get_config_path(confPath, sizeof(confPath), "hk-stream/workspaces") > 0) {
        const QString persistentPath = QDir::cleanPath(QString::fromUtf8(confPath));
        QDir().mkpath(persistentPath);

        // Migrate old builds' workspace files and assets once, without replacing
        // anything already in the persistent user directory.
        if (!QFileInfo::exists(QDir(persistentPath).filePath("workspace.json"))) {
            MigrateWorkspaceDirectory(cand1.absolutePath(), persistentPath);
        }
        return persistentPath.toStdString();
    }

    // Relative workspaces are a last resort, not the default for normal mode.
    QDir cand2("workspaces");
    if (cand2.exists())
        return cand2.canonicalPath().toStdString();

    cand1.mkpath(".");
    return cand1.absolutePath().toStdString();
}

std::string OBSWorkspaceManager::GetWorkspaceFilePath(const std::string &fileName)
{
    std::string dir = GetWorkspacesDirPath();
    QDir qdir(QString::fromStdString(dir));
    return qdir.filePath(QString::fromStdString(fileName)).toStdString();
}

std::string OBSWorkspaceManager::GetProjectFilePath(const std::string &sceneCollectionFileName)
{
	QDir projectsDir(QString::fromStdString(GetWorkspacesDirPath()));
	if (!projectsDir.mkpath("projects"))
		return {};
	projectsDir.cd("projects");
	QString collectionName = QFileInfo(QString::fromUtf8(sceneCollectionFileName.c_str())).completeBaseName();
	if (collectionName.isEmpty())
		collectionName = QStringLiteral("scene-collection");
	collectionName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
	const QByteArray identity = QCryptographicHash::hash(QByteArray::fromStdString(sceneCollectionFileName),
								     QCryptographicHash::Sha256).toHex().left(12);
	return projectsDir.filePath(collectionName + QLatin1Char('_') + QString::fromLatin1(identity) +
				     QStringLiteral(".hkproject")).toStdString();
}

std::string OBSWorkspaceManager::GetStylesDirPath()
{
    QDir workspaceDir(QString::fromStdString(GetWorkspacesDirPath()));
    const QString stylesDir = workspaceDir.filePath("styles");
    QDir().mkpath(QDir(stylesDir).filePath("assets/images"));
    QDir().mkpath(QDir(stylesDir).filePath("html"));
    return QDir::cleanPath(stylesDir).toStdString();
}

std::string OBSWorkspaceManager::GetStyleFilePath(const std::string &relativePath)
{
    QDir stylesDir(QString::fromStdString(GetStylesDirPath()));
    const QString path = stylesDir.filePath(QString::fromStdString(relativePath));
    QDir().mkpath(QFileInfo(path).absolutePath());
    return QDir::cleanPath(path).toStdString();
}

// -------------------------------------------------------
// Source Category Helpers
// -------------------------------------------------------
bool OBSWorkspaceManager::SetSourceCategory(const std::string &ws_id, const std::string &source_name, int category)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return false;

    const std::vector<std::string> previousCore = ws->core_sources;
    const std::vector<std::string> previousVariable = ws->variable_sources;

    const std::string logicalName = GetLogicalSourceName(source_name);
    auto matchesLogicalName = [&logicalName](const std::string &name) {
        return GetLogicalSourceName(name) == logicalName;
    };

    // Remove from both lists
    ws->core_sources.erase(
        std::remove_if(ws->core_sources.begin(), ws->core_sources.end(), matchesLogicalName),
        ws->core_sources.end());
    ws->variable_sources.erase(
        std::remove_if(ws->variable_sources.begin(), ws->variable_sources.end(), matchesLogicalName),
        ws->variable_sources.end());

    if (category == 0) {
        ws->core_sources.push_back(logicalName);
    } else if (category == 1) {
        ws->variable_sources.push_back(logicalName);
    }
    if (Save())
        return true;

    ws->core_sources = previousCore;
    ws->variable_sources = previousVariable;
    return false;
}

int OBSWorkspaceManager::GetSourceCategory(const std::string &ws_id, const std::string &source_name) const
{
    const std::string logicalName = GetLogicalSourceName(source_name);
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            const auto containsLogicalName = [&logicalName](const std::vector<std::string> &names) {
                return std::any_of(names.begin(), names.end(), [&logicalName](const std::string &name) {
                    return GetLogicalSourceName(name) == logicalName;
                });
            };
            if (containsLogicalName(ws.core_sources))
                return 0; // Core
            if (containsLogicalName(ws.variable_sources))
                return 1; // Variable
            return -1; // None
        }
    }
    return -1;
}

bool OBSWorkspaceManager::SetWorkspaceSources(const std::string &ws_id, const std::vector<std::string> &core, const std::vector<std::string> &variable)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return false;

    const std::vector<std::string> previousCore = ws->core_sources;
    const std::vector<std::string> previousVariable = ws->variable_sources;

    auto normalizeUnique = [](const std::vector<std::string> &input) {
        std::vector<std::string> result;
        for (const std::string &name : input) {
            std::string logicalName = GetLogicalSourceName(name);
            if (!logicalName.empty() && std::find(result.begin(), result.end(), logicalName) == result.end())
                result.push_back(std::move(logicalName));
        }
        return result;
    };

    ws->core_sources = normalizeUnique(core);
    ws->variable_sources = normalizeUnique(variable);
    ws->variable_sources.erase(
        std::remove_if(ws->variable_sources.begin(), ws->variable_sources.end(), [&ws](const std::string &name) {
            return std::find(ws->core_sources.begin(), ws->core_sources.end(), name) != ws->core_sources.end();
        }),
        ws->variable_sources.end());
    if (Save())
        return true;

    ws->core_sources = previousCore;
    ws->variable_sources = previousVariable;
    return false;
}

// -------------------------------------------------------
// Helpers
// -------------------------------------------------------
static std::vector<std::string> json_array_to_vec(json_t *arr)
{
    std::vector<std::string> result;
    if (!arr || !json_is_array(arr))
        return result;
    size_t idx;
    json_t *val;
    json_array_foreach(arr, idx, val) {
        if (json_is_string(val))
            result.emplace_back(json_string_value(val));
    }
    return result;
}

static json_t *vec_to_json_array(const std::vector<std::string> &v)
{
    json_t *arr = json_array();
    for (auto &s : v)
        json_array_append_new(arr, json_string(s.c_str()));
    return arr;
}

// -------------------------------------------------------
// Constructor
// -------------------------------------------------------
OBSWorkspaceManager::OBSWorkspaceManager() {}

// -------------------------------------------------------
// Load
// -------------------------------------------------------
void OBSWorkspaceManager::LoadDefaults()
{
    workspaces_.clear();
    active_id_ = "ws_live";

    OBSWorkspace wsLive;
    wsLive.id = "ws_live";
    wsLive.name = "بث مباشر";
    wsLive.icon = "broadcast";
    wsLive.active_scene = "بث مباشر";
    wsLive.scenes = {"بث مباشر", "عرض محتوى", "عرض يوتيوب"};
    wsLive.base_scene = "بث مباشر";
    wsLive.interactive_scenes["عرض يوتيوب"] = {"متصفح", "بث مباشر"};
    wsLive.core_sources = {"كمرة sony", "مايك الكمرة", "صوت سطح المكتب", "صوت داخل من الجوال", "عداد الوقت", "تنبيه الوقت", "تاريخ", "واجهة صورة", "صورة خلفية"};
    wsLive.variable_sources = {"العنوان", "تيكتوك", "تعليقات تيكت يوك", "دائرة يمين", "فيديو", "متصفح", "المتصفح", "غلاف يوتيوب", "الالتقاط النافذة 2"};
    workspaces_.push_back(std::move(wsLive));

    OBSWorkspace wsLesson;
    wsLesson.id = "ws_lesson";
    wsLesson.name = "درس";
    wsLesson.icon = "lesson";
    wsLesson.active_scene = "درس";
    wsLesson.base_scene = "درس";
    wsLesson.scenes = {"درس", "درس شاشة فقط"};
    wsLesson.core_sources = {"كمرة sony", "كمرة Anker", "مايك الكمرة", "صوت سطح المكتب", "صوت داخل من الجوال", "تاريخ", "تنبيه الوقت", "عداد الوقت", "واجهة صورة", "واجهة2", "صورة خلفية"};
    wsLesson.variable_sources = {"عنوان الدرس", "الترمذي", "نونية ابن القيم", "XODO", "pdf", "غلاف درس", "الصورة 2", "الترمذي1", "أدوبيpdf", "كتب الشاملة", "فيديو"};
    workspaces_.push_back(std::move(wsLesson));

    OBSWorkspace wsMisc;
    wsMisc.id = "ws_misc";
    wsMisc.name = "متنوعة";
    wsMisc.icon = "misc";
    wsMisc.active_scene = "شاشة";
    wsMisc.base_scene = "شاشة";
    wsMisc.scenes = {"كمرة سادة", "شاشة", "دسكورد", "طولي تعليق"};
    wsMisc.core_sources = {"كمرة sony", "صوت سطح المكتب", "التقاط جهاز العرض 2"};
    wsMisc.variable_sources = {"مـتصفح", "Edge", "شاشة دسكورد", "أدوبيpdf", "كتب الشاملة", "التقاط النافذة 5", "نص (GDI+)"};
    workspaces_.push_back(std::move(wsMisc));
}

// -------------------------------------------------------
// Load
// -------------------------------------------------------
bool OBSWorkspaceManager::Load(const std::string &path, const std::string &sceneCollectionPath,
			       const std::string &projectPath)
{
    std::string actual_path = path.empty() ? GetWorkspaceFilePath("workspace.json") : path;
    json_path_ = actual_path;
    scene_collection_path_ = sceneCollectionPath;
    project_path_ = projectPath;
    canonical_project_loaded_ = false;
	if (!project_path_.empty()) {
		std::string workspaceJson;
		std::string projectError;
		if (HKProject::LoadProjectWorkspace(project_path_, json_path_, scene_collection_path_, &workspaceJson,
							   &projectError) &&
		    ParseJson(workspaceJson.c_str()) && !workspaces_.empty()) {
			canonical_project_loaded_ = true;
			return true;
		}
		blog(LOG_WARNING, "[HKProject] Could not load canonical project %s: %s",
		     project_path_.c_str(), projectError.c_str());
	}
    auto loadFile = [this](const std::string &filePath) {
        std::ifstream f(filePath);
        if (!f.is_open()) {
            return false;
        }

        std::stringstream buf;
        buf << f.rdbuf();
        return ParseJson(buf.str().c_str()) && !workspaces_.empty();
    };

    if (loadFile(actual_path)) {
        return true;
    }

    const std::string backupPath = actual_path + ".bak";
    if (loadFile(backupPath)) {
        blog(LOG_WARNING, "[Workspaces] Recovered workspace settings from %s", backupPath.c_str());
        return true;
    }

    blog(LOG_WARNING, "[Workspaces] Could not load %s or its backup; loading defaults", actual_path.c_str());
    LoadDefaults();
    return false;
}

bool OBSWorkspaceManager::ParseJson(const char *json_str)
{
    if (!json_str) {
        LoadDefaults();
        return false;
    }

    // Strip UTF-8 BOM if present
    if ((unsigned char)json_str[0] == 0xEF &&
        (unsigned char)json_str[1] == 0xBB &&
        (unsigned char)json_str[2] == 0xBF) {
        json_str += 3;
    }

    json_error_t err;
    json_t *root = json_loads(json_str, 0, &err);
    if (!root) {
        blog(LOG_ERROR, "[Workspaces] JSON parse error: %s", err.text);
        LoadDefaults();
        return false;
    }

    workspaces_.clear();
    active_id_ = "";

    json_t *active_j = json_object_get(root, "active_workspace");
    if (json_is_string(active_j))
        active_id_ = json_string_value(active_j);

    json_t *list = json_object_get(root, "workspaces");
    if (!json_is_array(list)) {
        json_decref(root);
        LoadDefaults();
        return false;
    }

    size_t idx;
    json_t *ws_j;
    json_array_foreach(list, idx, ws_j) {
        OBSWorkspace ws;
        auto get_str = [&](const char *k) -> std::string {
            json_t *v = json_object_get(ws_j, k);
            return json_is_string(v) ? json_string_value(v) : "";
        };
        ws.id   = get_str("id");
        ws.name = get_str("name");
        ws.icon = get_str("icon");
        ws.active_scene = get_str("active_scene");
        json_t *interactiveMode = json_object_get(ws_j, "interactive_mode_enabled");
        const bool hasInteractiveMode = json_is_boolean(interactiveMode);
        if (hasInteractiveMode)
            ws.interactive_mode_enabled = json_is_true(interactiveMode);
        ws.base_scene = get_str("base_scene");
        ws.scenes = json_array_to_vec(json_object_get(ws_j, "scenes"));
        ws.hidden_scenes = json_array_to_vec(json_object_get(ws_j, "hidden_scenes"));
        json_t *hba_j = json_object_get(ws_j, "hidden_scenes_by_aspect");
        if (json_is_object(hba_j)) {
            const char *asp_k;
            json_t *arr_v;
            json_object_foreach(hba_j, asp_k, arr_v) {
                if (json_is_array(arr_v)) {
                    ws.hidden_scenes_by_aspect[asp_k] = json_array_to_vec(arr_v);
                }
            }
        }

        json_t *colors_j = json_object_get(ws_j, "scene_text_colors");
        if (json_is_object(colors_j)) {
            const char *sc_name;
            json_t *elems_j;
            json_object_foreach(colors_j, sc_name, elems_j) {
                if (json_is_object(elems_j)) {
                    const char *elem_k;
                    json_t *col_v;
                    json_object_foreach(elems_j, elem_k, col_v) {
                        if (json_is_string(col_v)) {
                            ws.scene_text_colors[sc_name][elem_k] = json_string_value(col_v);
                        }
                    }
                }
            }
        }

        json_t *inter_j = json_object_get(ws_j, "interactive_scenes");
        if (json_is_object(inter_j)) {
            const char *sc_k;
            json_t *config_v;
            json_object_foreach(inter_j, sc_k, config_v) {
                OBSInteractiveSceneConfig config;
                if (json_is_string(config_v)) {
                    // Migrate the previous scene -> source format without changing
                    // its existing behavior: inactivity returned to the base scene.
                    config.source_name = json_string_value(config_v);
                    config.fallback_scene = ws.base_scene;
                } else if (json_is_object(config_v)) {
                    json_t *source_v = json_object_get(config_v, "source");
                    json_t *fallback_v = json_object_get(config_v, "fallback");
                    if (json_is_string(source_v))
                        config.source_name = json_string_value(source_v);
                    if (json_is_string(fallback_v))
                        config.fallback_scene = json_string_value(fallback_v);
                    json_t *enabled_v = json_object_get(config_v, "enabled");
                    if (json_is_boolean(enabled_v))
                        config.enabled = json_is_true(enabled_v);
                    if (config.fallback_scene.empty())
                        config.fallback_scene = ws.base_scene;
                }
                if (!config.source_name.empty() && !config.fallback_scene.empty() &&
                    config.fallback_scene != sc_k)
                    ws.interactive_scenes[sc_k] = std::move(config);
            }
        }
        if (!hasInteractiveMode && !ws.interactive_scenes.empty()) {
            ws.interactive_mode_enabled = std::any_of(
                ws.interactive_scenes.begin(), ws.interactive_scenes.end(),
                [](const auto &entry) { return entry.second.enabled; });
        }

        json_t *video_j = json_object_get(ws_j, "video");
        if (json_is_object(video_j)) {
            auto get_u = [&](const char *k, uint32_t def) -> uint32_t {
                json_t *v = json_object_get(video_j, k);
                return json_is_integer(v) ? (uint32_t)json_integer_value(v) : def;
            };
            ws.video.base_cx   = get_u("base_cx",   1920);
            ws.video.base_cy   = get_u("base_cy",   1080);
            ws.video.output_cx = get_u("output_cx", 1920);
            ws.video.output_cy = get_u("output_cy", 1080);
            ws.video.fps_num   = get_u("fps_num",   30);
            ws.video.fps_den   = get_u("fps_den",   1);
        }

        json_t *sources_j = json_object_get(ws_j, "sources");
        if (json_is_object(sources_j)) {
            ws.core_sources     = json_array_to_vec(json_object_get(sources_j, "core"));
            ws.variable_sources = json_array_to_vec(json_object_get(sources_j, "variable"));
        }

        json_t *scene_res_j = json_object_get(ws_j, "scene_resolutions");
        if (json_is_object(scene_res_j)) {
            const char *s_key;
            json_t *s_val;
            json_object_foreach(scene_res_j, s_key, s_val) {
                if (json_is_object(s_val)) {
                    uint32_t scx = (uint32_t)json_integer_value(json_object_get(s_val, "cx"));
                    uint32_t scy = (uint32_t)json_integer_value(json_object_get(s_val, "cy"));
                    if (scx > 0 && scy > 0) {
                        ws.scene_resolutions[s_key] = {scx, scy};
                    }
                }
            }
        }

        json_t *layouts_j = json_object_get(ws_j, "scene_layouts");
        if (json_is_object(layouts_j)) {
            const char *sc_name;
            json_t *aspects_j;
            json_object_foreach(layouts_j, sc_name, aspects_j) {
                if (json_is_object(aspects_j)) {
                    const char *asp_key;
                    json_t *items_j;
                    json_object_foreach(aspects_j, asp_key, items_j) {
                        if (json_is_array(items_j)) {
                            std::vector<SceneItemTransform> items;
                            size_t it_idx;
                            json_t *item_j;
                            json_array_foreach(items_j, it_idx, item_j) {
                                if (json_is_object(item_j)) {
                                    SceneItemTransform t;
                                    auto get_str_val = [&](const char *k) {
                                        json_t *v = json_object_get(item_j, k);
                                        return json_is_string(v) ? json_string_value(v) : "";
                                    };
                                    auto get_real_val = [&](const char *k, double def = 0.0) {
                                        json_t *v = json_object_get(item_j, k);
                                        return json_is_number(v) ? json_number_value(v) : def;
                                    };
                                    auto get_int_val = [&](const char *k, int64_t def = 0) {
                                        json_t *v = json_object_get(item_j, k);
                                        return json_is_integer(v) ? json_integer_value(v) : def;
                                    };
                                    auto get_bool_val = [&](const char *k, bool def = true) {
                                        json_t *v = json_object_get(item_j, k);
                                        return json_is_boolean(v) ? json_is_true(v) : def;
                                    };
                                    t.name = get_str_val("name");
                                    t.id = get_int_val("id");
                                    t.pos_x = (float)get_real_val("pos_x", 0.0);
                                    t.pos_y = (float)get_real_val("pos_y", 0.0);
                                    t.scale_x = (float)get_real_val("scale_x", 1.0);
                                    t.scale_y = (float)get_real_val("scale_y", 1.0);
                                    t.rot = (float)get_real_val("rot", 0.0);
                                    t.alignment = (uint32_t)get_int_val("alignment", 5);
                                    t.bounds_type = (uint32_t)get_int_val("bounds_type", 0);
                                    t.bounds_alignment = (uint32_t)get_int_val("bounds_alignment", 0);
                                    t.bounds_x = (float)get_real_val("bounds_x", 0.0);
                                    t.bounds_y = (float)get_real_val("bounds_y", 0.0);
                                    t.crop_to_bounds = get_bool_val("crop_to_bounds", false);
                                    t.crop_left = (int)get_int_val("crop_left", 0);
                                    t.crop_top = (int)get_int_val("crop_top", 0);
                                    t.crop_right = (int)get_int_val("crop_right", 0);
                                    t.crop_bottom = (int)get_int_val("crop_bottom", 0);
                                    t.visible = get_bool_val("visible", true);
                                    items.push_back(std::move(t));
                                }
                            }
                            ws.scene_layouts[sc_name][asp_key] = std::move(items);
                        }
                    }
                }
            }
        }

        workspaces_.push_back(std::move(ws));
    }

    json_decref(root);
    blog(LOG_INFO, "[Workspaces] Loaded %zu workspaces. Active: %s",
         workspaces_.size(), active_id_.c_str());
    return true;
}

void OBSWorkspaceManager::SetWorkspaceResolution(const std::string &ws_id, uint32_t cx, uint32_t cy)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (ws) {
        ws->video.base_cx = cx;
        ws->video.base_cy = cy;
        ws->video.output_cx = cx;
        ws->video.output_cy = cy;
        Save();
    }
}

void OBSWorkspaceManager::SetSceneResolution(const std::string &ws_id, const std::string &scene_name, uint32_t cx, uint32_t cy)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (ws && !scene_name.empty()) {
        ws->scene_resolutions[scene_name] = {cx, cy};
        ws->video.base_cx = cx;
        ws->video.base_cy = cy;
        ws->video.output_cx = cx;
        ws->video.output_cy = cy;
        Save();
    }
}

bool OBSWorkspaceManager::GetSceneResolution(const std::string &ws_id, const std::string &scene_name, uint32_t &cx, uint32_t &cy) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            auto it = ws.scene_resolutions.find(scene_name);
            if (it != ws.scene_resolutions.end()) {
                cx = it->second.first;
                cy = it->second.second;
                return true;
            }
            break;
        }
    }
    return false;
}

std::string OBSWorkspaceManager::GetAspectRatioKey(uint32_t cx, uint32_t cy)
{
    if (cx == 0 || cy == 0) return "16:9";
    double r = static_cast<double>(cx) / static_cast<double>(cy);
    if (r >= 1.6) return "16:9";
    if (r <= 0.65) return "9:16";
    if (r >= 1.25 && r <= 1.45) return "4:3";
    if (r >= 0.75 && r <= 0.85) return "4:5";
    if (std::abs(r - 1.0) < 0.1) return "1:1";
    return std::to_string(cx) + "x" + std::to_string(cy);
}

bool OBSWorkspaceManager::IsAspectKey(const std::string &str)
{
    if (str == "16:9" || str == "9:16" || str == "1:1" || str == "4:5" || str == "4:3") return true;
    size_t xpos = str.find('x');
    if (xpos != std::string::npos && xpos > 0 && xpos < str.size() - 1) {
        for (size_t i = 0; i < str.size(); ++i) {
            if (i == xpos) continue;
            if (!isdigit(static_cast<unsigned char>(str[i]))) return false;
        }
        return true;
    }
    size_t colon = str.find(':');
    if (colon != std::string::npos && colon > 0 && colon < str.size() - 1) {
        for (size_t i = 0; i < str.size(); ++i) {
            if (i == colon) continue;
            if (!isdigit(static_cast<unsigned char>(str[i]))) return false;
        }
        return true;
    }
    return false;
}

std::string OBSWorkspaceManager::GetActiveAspectRatioKey()
{
    obs_source_t *cur = obs_frontend_get_current_scene();
    if (cur) {
        std::string name = obs_source_get_name(cur);
        obs_source_release(cur);
        size_t bracketPos = name.rfind(" [");
        if (bracketPos != std::string::npos && name.back() == ']') {
            std::string asp = name.substr(bracketPos + 2, name.size() - bracketPos - 3);
            if (IsAspectKey(asp)) {
                return asp;
            }
        }
    }
    obs_video_info ovi = {};
    if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
        return GetAspectRatioKey(ovi.base_width, ovi.base_height);
    }
    return "16:9";
}

std::string OBSWorkspaceManager::GetLogicalSceneName(const std::string &name)
{
	std::string logicalName = name;
	auto isSizeKey = [](const std::string &tag) {
		if (tag.rfind("SIZE:", 0) != 0) return false;
		const size_t x = tag.find('x', 5);
		if (x == std::string::npos || x == 5 || x + 1 >= tag.size()) return false;
		return std::all_of(tag.begin() + 5, tag.begin() + x, [](unsigned char c) { return std::isdigit(c); }) &&
		       std::all_of(tag.begin() + x + 1, tag.end(), [](unsigned char c) { return std::isdigit(c); });
	};
	auto stripTrailingTag = [&logicalName](const std::string &tagPrefix, const auto &isValid) {
		if (logicalName.empty() || logicalName.back() != ']') return;
		size_t bracketPos = logicalName.rfind(" [");
		if (bracketPos == std::string::npos) return;
		std::string tag = logicalName.substr(bracketPos + 2, logicalName.size() - bracketPos - 3);
		if (tag.rfind(tagPrefix, 0) == 0 && isValid(tag)) {
			logicalName.resize(bracketPos);
		}
	};
	stripTrailingTag("SIZE:", isSizeKey);
	stripTrailingTag("", [](const std::string &tag) { return IsAspectKey(tag); });
	stripTrailingTag("WS:", [](const std::string &tag) { return tag.size() > 3; });
	return logicalName;
}

std::string OBSWorkspaceManager::GetLogicalSourceName(const std::string &name)
{
	if (name.empty()) return name;

	const size_t suffixStart = name.find(" [");
	if (suffixStart == std::string::npos) return name;

	const std::string suffix = name.substr(suffixStart);
	const bool hasWorkspaceTag = suffix.find("[WS:") != std::string::npos || suffix.find("[SIZE:") != std::string::npos;
	const bool hasAspectTag = suffix.find("[16:9]") != std::string::npos || suffix.find("[9:16]") != std::string::npos ||
		suffix.find("[4:3]") != std::string::npos || suffix.find("[4:5]") != std::string::npos ||
		suffix.find("[1:1]") != std::string::npos;

	if (hasWorkspaceTag || hasAspectTag) {
		return name.substr(0, suffixStart);
	}

	return name;
}

std::string OBSWorkspaceManager::GetBackingSceneName(const std::string &logicalName, const std::string &aspect_key,
							     const std::string &workspace_id, uint32_t cx, uint32_t cy)
{
	if (!workspace_id.empty()) {
		if (cx && cy)
			return logicalName + " [WS:" + workspace_id + "] [SIZE:" + std::to_string(cx) + "x" +
			       std::to_string(cy) + "]";
		return logicalName + " [WS:" + workspace_id + "] [" + aspect_key + "]";
	}
	if (aspect_key == "16:9") {
        std::string explicit169 = logicalName + " [16:9]";
        obs_source_t *s = obs_get_source_by_name(explicit169.c_str());
        if (s) {
            obs_source_release(s);
            return explicit169;
        }
        return logicalName;
    }
    return logicalName + " [" + aspect_key + "]";
}

namespace {
bool IsBrowserSourceId(const char *sourceId)
{
	return sourceId && (strcmp(sourceId, "browser_source") == 0 ||
			    strcmp(sourceId, "embedded_html_source") == 0);
}

bool CanIsolateSource(const obs_source_t *source)
{
	return source && (obs_source_get_output_flags(source) & OBS_SOURCE_DO_NOT_DUPLICATE) == 0;
}

bool IsSceneTextColorBrowserSource(const obs_source_t *source)
{
	if (!source || !IsBrowserSourceId(obs_source_get_id(source)))
		return false;

	const char *sourceName = obs_source_get_name(source);
	if (!sourceName)
		return false;

	const std::string name(sourceName);
	return name.find("تاريخ") != std::string::npos || name.find("التاريخ") != std::string::npos ||
	       name.find("تنبيه الوقت") != std::string::npos || name.find("تنبيه وقت") != std::string::npos;
}

void RestoreCopiedSceneItemBorder(obs_source_t *renderSource)
{
	if (!renderSource || strcmp(obs_source_get_id(renderSource), "hk_scene_item_render_source") != 0)
		return;

	obs_source_t *filter = obs_source_get_filter_by_name(renderSource, "حدود HK");
	if (!filter)
		return;

	obs_data_t *settings = obs_source_get_settings(filter);
	if (settings) {
		const bool shouldBeEnabled = obs_data_get_bool(settings, "border_enabled") ||
					     obs_data_get_bool(settings, "shadow_enabled");
		if (shouldBeEnabled && !obs_source_enabled(filter))
			obs_source_set_enabled(filter, true);
		obs_data_release(settings);
	}
	obs_source_release(filter);
}

void CopySceneItemProcessing(obs_sceneitem_t *destination, obs_sceneitem_t *original)
{
	obs_data_t *originalPrivate = obs_sceneitem_get_private_settings(original);
	obs_data_t *destinationPrivate = obs_sceneitem_get_private_settings(destination);
	obs_data_apply(destinationPrivate, originalPrivate);
	obs_data_release(destinationPrivate);
	obs_data_release(originalPrivate);

	obs_source_t *from = obs_sceneitem_get_filter_source(original);
	obs_source_t *to = obs_sceneitem_get_filter_source(destination);
	if (from && to && strcmp(obs_source_get_id(from), "hk_scene_item_render_source") == 0 &&
	    strcmp(obs_source_get_id(to), "hk_scene_item_render_source") == 0) {
		obs_data_t *fromPrivate = obs_source_get_private_settings(from);
		obs_data_t *toPrivate = obs_source_get_private_settings(to);
		obs_data_apply(toPrivate, fromPrivate);
		obs_data_release(toPrivate);
		obs_data_release(fromPrivate);

		// Enumeration holds the filter lock. Collect references, then remove
		// inherited defaults outside that lock before copying the item's filters.
		std::vector<obs_source_t *> defaults;
		obs_source_enum_filters(to, [](obs_source_t *, obs_source_t *filter, void *param) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(filter));
		}, &defaults);
		for (obs_source_t *filter : defaults) {
			obs_source_filter_remove(to, filter);
			obs_source_release(filter);
		}
		obs_source_copy_filters(to, from);
		// Replacing or cloning a scene item copies the filter's enabled bit too.
		// Older saved items can have that bit off while their HK border/shadow
		// settings are on, so normalize the copied per-item filter after the copy.
		RestoreCopiedSceneItemBorder(to);
	}
	if (to) obs_source_release(to);
	if (from) obs_source_release(from);
}

obs_sceneitem_t *CopySceneItem(obs_scene_t *destination, obs_sceneitem_t *original, obs_source_t *source)
{
	if (!destination || !original || !source)
		return nullptr;
	obs_sceneitem_t *newItem = obs_scene_add(destination, source);
	if (!newItem)
		return nullptr;
	obs_transform_info info{};
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_info2(original, &info);
	obs_sceneitem_get_crop(original, &crop);
	obs_sceneitem_set_info2(newItem, &info);
	obs_sceneitem_set_crop(newItem, &crop);
	obs_sceneitem_set_visible(newItem, obs_sceneitem_visible(original));
	obs_sceneitem_set_locked(newItem, obs_sceneitem_locked(original));
	obs_sceneitem_set_id(newItem, obs_sceneitem_get_id(original));
	obs_sceneitem_set_scale_filter(newItem, obs_sceneitem_get_scale_filter(original));
	obs_sceneitem_set_blending_method(newItem, obs_sceneitem_get_blending_method(original));
	obs_sceneitem_set_blending_mode(newItem, obs_sceneitem_get_blending_mode(original));
	obs_sceneitem_set_order_position(newItem, obs_sceneitem_get_order_position(original));
	CopySceneItemProcessing(newItem, original);
	return newItem;
}

void CopySceneItems(obs_scene_t *destination, obs_scene_t *original)
{
	if (!destination || !original)
		return;
	struct CopyData {
		obs_scene_t *destination;
	} copyData{destination};
	obs_scene_enum_items(original, [](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
		auto *data = static_cast<CopyData *>(param);
		obs_source_t *source = obs_sceneitem_get_source(item);
		CopySceneItem(data->destination, item, source);
		return true;
	}, &copyData);
}

bool SceneHasItemId(obs_scene_t *scene, int64_t id)
{
	bool found = false;
	std::pair<int64_t, bool *> check{id, &found};
	obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
		auto *lookup = static_cast<std::pair<int64_t, bool *> *>(param);
		if (obs_sceneitem_get_id(item) == lookup->first) {
			*lookup->second = true;
			return false;
		}
		return true;
	}, &check);
	return found;
}

bool RestoreMissingSceneItems(obs_scene_t *destination, obs_scene_t *original, const std::string &backingName)
{
	if (!destination || !original)
		return false;
	bool restoredAny = false;
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(original, [](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
		auto *collected = static_cast<std::vector<obs_sceneitem_t *> *>(param);
		obs_sceneitem_addref(item);
		collected->push_back(item);
		return true;
	}, &items);

	for (obs_sceneitem_t *item : items) {
		const int64_t itemId = obs_sceneitem_get_id(item);
		const bool isGroup = obs_sceneitem_is_group(item);
		obs_source_t *source = obs_sceneitem_get_source(item);
		const bool isNestedScene = source && obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !isGroup;
		if (!SceneHasItemId(destination, itemId)) {
			if (isGroup || isNestedScene) {
				obs_scene_t *sourceScene = isGroup ? obs_sceneitem_group_get_scene(item) : obs_scene_from_source(source);
				if (sourceScene) {
					const std::string cloneName = (isGroup ? "HK Group [" : "HK Scene [") + backingName +"][" + std::to_string(itemId) + "]";
					obs_scene_t *cloneScene = obs_scene_duplicate(sourceScene, cloneName.c_str(), OBS_SCENE_DUP_COPY);
					if (cloneScene) {
						obs_source_t *cloneSource = obs_source_get_ref(obs_scene_get_source(cloneScene));
						if (cloneSource) {
							obs_data_t *privateSettings = obs_source_get_private_settings(cloneSource);
							if (privateSettings) {
								obs_data_set_string(privateSettings, "hk_layout_backing", backingName.c_str());
								obs_data_release(privateSettings);
							}
							if (CopySceneItem(destination, item, cloneSource)) {
								restoredAny = true;
								blog(LOG_WARNING, "[Workspace] Restored missing %s '%s' into '%s'",
								     isGroup ? "group" : "nested scene", obs_source_get_name(source), backingName.c_str());
							}
							obs_source_release(cloneSource);
						}
						obs_scene_release(cloneScene);
					}
				}
			} else if (source && CopySceneItem(destination, item, source)) {
				restoredAny = true;
				blog(LOG_WARNING, "[Workspace] Restored missing scene item '%s' into '%s'",
				     obs_source_get_name(source), backingName.c_str());
			}
		}
		obs_sceneitem_release(item);
	}
	return restoredAny;
}

struct CameraCropMigration {
	obs_source_t *camera = nullptr;
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
	bool foundItems = false;
	bool changedItems = false;
	bool incomplete = false;
	std::vector<obs_scene_t *> visited;
};

void MigrateCameraCropInScene(obs_scene_t *scene, CameraCropMigration &migration)
{
	if (!scene || std::find(migration.visited.begin(), migration.visited.end(), scene) != migration.visited.end())
		return;
	migration.visited.push_back(scene);

	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
		auto *collected = static_cast<std::vector<obs_sceneitem_t *> *>(param);
		obs_sceneitem_addref(item);
		collected->push_back(item);
		return true;
	}, &items);

	for (obs_sceneitem_t *item : items) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source == migration.camera) {
			migration.foundItems = true;
			obs_data_t *privateSettings = obs_sceneitem_get_private_settings(item);
			if (!privateSettings) {
				migration.incomplete = true;
			} else {
				obs_sceneitem_crop crop{};
				obs_sceneitem_get_crop(item, &crop);
				const bool alreadyMigrated = obs_data_get_bool(privateSettings, "hk_shared_camera_crop_v1");
				if (!alreadyMigrated) {
					crop.left += migration.left;
					crop.top += migration.top;
					crop.right += migration.right;
					crop.bottom += migration.bottom;
					obs_sceneitem_set_crop(item, &crop);
					obs_data_set_bool(privateSettings, "hk_shared_camera_crop_v1", true);
					migration.changedItems = true;
				} else if (crop.left == 0 && crop.top == 0 && crop.right == 0 && crop.bottom == 0 &&
					   (migration.left != 0 || migration.top != 0 || migration.right != 0 ||
					    migration.bottom != 0)) {
					// The legacy timer script moved this item's crop into the shared
					// source filter and then zeroed the item crop. Restore that crop
					// before removing the filter, without adding it twice to items
					// that still have their scene-local crop.
					crop.left = migration.left;
					crop.top = migration.top;
					crop.right = migration.right;
					crop.bottom = migration.bottom;
					obs_sceneitem_set_crop(item, &crop);
					migration.changedItems = true;
				}
			}
			if (privateSettings)
				obs_data_release(privateSettings);
		} else if (obs_sceneitem_is_group(item)) {
			MigrateCameraCropInScene(obs_sceneitem_group_get_scene(item), migration);
		} else if (source && obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
			MigrateCameraCropInScene(obs_scene_from_source(source), migration);
		}
		obs_sceneitem_release(item);
	}
}

obs_sceneitem_t *ReplaceSceneItemSource(obs_scene_t *scene, obs_sceneitem_t *oldItem, obs_source_t *newSource)
{
	if (!scene || !oldItem || !newSource) return nullptr;

	obs_sceneitem_t *newItem = obs_scene_add(scene, newSource);
	if (!newItem) return nullptr;

	obs_transform_info info{};
	obs_sceneitem_get_info2(oldItem, &info);
	obs_sceneitem_set_info2(newItem, &info);
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_crop(oldItem, &crop);
	obs_sceneitem_set_crop(newItem, &crop);
	obs_sceneitem_set_visible(newItem, obs_sceneitem_visible(oldItem));
	obs_sceneitem_set_locked(newItem, obs_sceneitem_locked(oldItem));
	obs_sceneitem_set_id(newItem, obs_sceneitem_get_id(oldItem));
	obs_sceneitem_set_scale_filter(newItem, obs_sceneitem_get_scale_filter(oldItem));
	obs_sceneitem_set_blending_method(newItem, obs_sceneitem_get_blending_method(oldItem));
	obs_sceneitem_set_blending_mode(newItem, obs_sceneitem_get_blending_mode(oldItem));
	obs_sceneitem_set_order_position(newItem, obs_sceneitem_get_order_position(oldItem));
	CopySceneItemProcessing(newItem, oldItem);
	obs_sceneitem_remove(oldItem);
	return newItem;
}

void IsolateDesignSources(obs_scene_t *scene, const std::string &backingName)
{
	if (!scene) return;

	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
		auto *collected = static_cast<std::vector<obs_sceneitem_t *> *>(param);
		obs_sceneitem_addref(item);
		collected->push_back(item);
		return true;
	}, &items);

	for (obs_sceneitem_t *item : items) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		bool isGroup = obs_sceneitem_is_group(item);
		bool isNestedScene = source && obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !isGroup;
		if (isGroup || isNestedScene) {
			obs_data_t *privateSettings = obs_source_get_private_settings(source);
			const char *sourceBacking = privateSettings ? obs_data_get_string(privateSettings, "hk_layout_backing") : "";
			bool alreadyIsolated = sourceBacking && backingName == sourceBacking;
			if (privateSettings) obs_data_release(privateSettings);

			if (!alreadyIsolated) {
				std::string cloneName = std::string(isGroup ? "HK Group [" : "HK Scene [") + backingName + "]";
				obs_scene_t *sourceScene = isGroup ? obs_sceneitem_group_get_scene(item) : obs_scene_from_source(source);
				obs_scene_t *clonedScene = sourceScene
						   ? obs_scene_duplicate(sourceScene, cloneName.c_str(), OBS_SCENE_DUP_COPY)
						   : nullptr;
				obs_source_t *clone = clonedScene ? obs_source_get_ref(obs_scene_get_source(clonedScene)) : nullptr;
				if (clone) {
					obs_data_t *clonePrivateSettings = obs_source_get_private_settings(clone);
					if (clonePrivateSettings) {
						obs_data_set_string(clonePrivateSettings, "hk_layout_backing", backingName.c_str());
						obs_data_release(clonePrivateSettings);
					}
					obs_sceneitem_t *newItem = ReplaceSceneItemSource(scene, item, clone);
					if (newItem) {
						obs_source_t *newItemSource = obs_sceneitem_get_source(newItem);
						obs_scene_t *nestedScene = isGroup ? obs_sceneitem_group_get_scene(newItem)
										 : obs_scene_from_source(newItemSource);
						IsolateDesignSources(nestedScene, backingName);
					}
					obs_source_release(clone);
				}
				if (clonedScene)
					obs_scene_release(clonedScene);
			} else {
				obs_scene_t *nestedScene = isGroup ? obs_sceneitem_group_get_scene(item) : obs_scene_from_source(source);
				IsolateDesignSources(nestedScene, backingName);
			}
		} else if (CanIsolateSource(source) || IsSceneTextColorBrowserSource(source)) {
			const char *sourceName = obs_source_get_name(source);
			const char *sourceId = obs_source_get_id(source);
			const bool isolateTextBrowserSource = IsSceneTextColorBrowserSource(source);
			obs_data_t *privateSettings = obs_source_get_private_settings(source);
			const char *sourceBacking = privateSettings ? obs_data_get_string(privateSettings, "hk_layout_backing") : "";
			bool alreadyIsolated = sourceBacking && backingName == sourceBacking;
			if (privateSettings) obs_data_release(privateSettings);

			if (alreadyIsolated) {
				obs_sceneitem_release(item);
				continue;
			}

			std::string cloneName = std::string(sourceName ? sourceName : "Text") +
					       " [" + backingName + "]";

			if (sourceName && cloneName != sourceName) {
				obs_source_t *clone = obs_get_source_by_name(cloneName.c_str());
				if (clone && strcmp(obs_source_get_id(clone), sourceId) != 0) {
					obs_source_release(clone);
					clone = nullptr;
				}
				if (!clone) {
					if (isolateTextBrowserSource) {
						// Browser sources opt out of obs_source_duplicate because OBS treats
						// them as shared live pages. Scene-specific CSS still needs a separate
						// instance, so create one from the same settings explicitly.
						obs_data_t *sourceSettings = obs_source_get_settings(source);
						clone = obs_source_create(sourceId, cloneName.c_str(), sourceSettings, nullptr);
						if (sourceSettings)
							obs_data_release(sourceSettings);
						if (clone) {
							obs_data_t *originalPrivate = obs_source_get_private_settings(source);
						obs_data_t *clonePrivate = obs_source_get_private_settings(clone);
						if (originalPrivate && clonePrivate)
							obs_data_apply(clonePrivate, originalPrivate);
						if (clonePrivate)
							obs_data_release(clonePrivate);
						if (originalPrivate)
							obs_data_release(originalPrivate);
						}
					} else {
						clone = obs_source_duplicate(source, cloneName.c_str(), false);
					}
				}
				if (clone && strcmp(obs_source_get_name(clone), sourceName) != 0) {
					obs_data_t *clonePrivateSettings = obs_source_get_private_settings(clone);
					if (clonePrivateSettings) {
						obs_data_set_string(clonePrivateSettings, "hk_layout_backing", backingName.c_str());
						obs_data_release(clonePrivateSettings);
					}
					ReplaceSceneItemSource(scene, item, clone);
				}
				if (clone) obs_source_release(clone);
			}
		}
		obs_sceneitem_release(item);
	}
}
} // namespace

bool OBSWorkspaceManager::MigrateSharedCameraCropToSceneItems()
{
	// This is deliberately limited to the identified shared SONY capture source
	// and its known crop filter. Other capture devices/filters remain untouched.
	constexpr const char *cameraName = "كمرة sony";
	constexpr const char *cameraUuid = "375f685f-1262-4bcb-849b-a9ed03d0df2e";
	constexpr const char *filterName = "قص حدود HK";

	obs_source_t *camera = obs_get_source_by_name(cameraName);
	if (!camera)
		return false;
	if (strcmp(obs_source_get_uuid(camera), cameraUuid) != 0 || strcmp(obs_source_get_id(camera), "dshow_input") != 0) {
		obs_source_release(camera);
		blog(LOG_WARNING, "[Workspace] Skipped camera crop migration: the source identity did not match the saved SONY camera");
		return false;
	}

	obs_source_t *filter = obs_source_get_filter_by_name(camera, filterName);
	if (!filter) {
		obs_source_release(camera);
		return false;
	}
	if (strcmp(obs_source_get_id(filter), "crop_filter") != 0 || !obs_source_enabled(filter)) {
		obs_source_release(filter);
		obs_source_release(camera);
		return false;
	}

	obs_data_t *filterSettings = obs_source_get_settings(filter);
	const bool relative = filterSettings && obs_data_get_bool(filterSettings, "relative");
	if (!relative) {
		blog(LOG_WARNING, "[Workspace] Kept the SONY camera crop filter because its crop mode is not relative");
		if (filterSettings)
			obs_data_release(filterSettings);
		obs_source_release(filter);
		obs_source_release(camera);
		return false;
	}

	CameraCropMigration migration;
	migration.camera = camera;
	migration.left = static_cast<int>(obs_data_get_int(filterSettings, "left"));
	migration.top = static_cast<int>(obs_data_get_int(filterSettings, "top"));
	migration.right = static_cast<int>(obs_data_get_int(filterSettings, "right"));
	migration.bottom = static_cast<int>(obs_data_get_int(filterSettings, "bottom"));
	obs_data_release(filterSettings);

	obs_enum_scenes([](void *opaque, obs_source_t *sceneSource) {
		auto *state = static_cast<CameraCropMigration *>(opaque);
		if (obs_scene_t *scene = obs_scene_from_source(sceneSource))
			MigrateCameraCropInScene(scene, *state);
		return true;
	}, &migration);

	bool removedSharedFilter = false;
	if (migration.foundItems && !migration.incomplete) {
		obs_source_filter_remove(camera, filter);
		removedSharedFilter = true;
		blog(LOG_WARNING,
		     "[Workspace] Migrated/restored shared SONY crop L=%d T=%d R=%d B=%d into scene items; removed the shared crop filter",
		     migration.left, migration.top, migration.right, migration.bottom);
	} else if (migration.foundItems) {
		blog(LOG_ERROR, "[Workspace] Kept the shared SONY crop filter because at least one scene item could not be migrated");
	}

	obs_source_release(filter);
	obs_source_release(camera);
	return migration.changedItems || removedSharedFilter;
}

std::string OBSWorkspaceManager::GetOrCreateBackingScene(const std::string &logicalName, const std::string &aspect_key,
								 const std::string &workspace_id, uint32_t requestedCX, uint32_t requestedCY,
								 const std::string &templateWorkspaceId)
{
    if (logicalName.empty()) return "";

	uint32_t targetCX = requestedCX;
	uint32_t targetCY = requestedCY;
	if (!targetCX || !targetCY) {
		targetCX = 1920;
		targetCY = 1080;
		if (aspect_key == "9:16") { targetCX = 1080; targetCY = 1920; }
		else if (aspect_key == "4:3") { targetCX = 1440; targetCY = 1080; }
		else if (aspect_key == "4:5") { targetCX = 1080; targetCY = 1350; }
		else if (aspect_key == "1:1") { targetCX = 1080; targetCY = 1080; }
	}
	std::string targetBacking = GetBackingSceneName(logicalName, aspect_key, workspace_id, targetCX, targetCY);

    // 1. Check if it already exists in OBS
    obs_source_t *targetSrc = obs_get_source_by_name(targetBacking.c_str());
    if (targetSrc) {
        // Ensure custom size is set correctly on existing backing scene
        obs_scene_t *sc = obs_scene_from_source(targetSrc);
        if (sc) {
            obs_scene_set_custom_size(sc, true, targetCX, targetCY);
        }
        obs_scene_t *existingScene = obs_scene_from_source(targetSrc);
		// These legacy lesson scenes contain the saved designs. Earlier versions
		// created incomplete wide backings because the originals inherit the
		// collection resolution instead of storing custom_size. Merge missing item
		// IDs into the backing without replacing any of the user's saved edits.
		const bool isWideLesson = workspace_id == "ws_lesson" && aspect_key == "16:9" &&
			targetCX == 1920 && targetCY == 1080 &&
			(logicalName == "درس" || logicalName == "درس شاشة فقط");
		if (isWideLesson && existingScene) {
			obs_video_info video{};
			obs_source_t *legacySource = obs_get_source_by_name(logicalName.c_str());
			obs_scene_t *legacyScene = legacySource ? obs_scene_from_source(legacySource) : nullptr;
			obs_data_t *restoreState = obs_source_get_private_settings(targetSrc);
			const bool matchingCanvas = obs_get_video_info(&video) && video.base_width == targetCX &&
				video.base_height == targetCY;
			if (matchingCanvas && legacyScene) {
				RestoreMissingSceneItems(existingScene, legacyScene, targetBacking);
				if (restoreState)
					obs_data_set_bool(restoreState, "hk_lesson_wide_restored", true);
			}
			if (restoreState)
				obs_data_release(restoreState);
			if (legacySource)
				obs_source_release(legacySource);
		}
        IsolateDesignSources(existingScene, targetBacking);
        obs_source_release(targetSrc);
        return targetBacking;
    }

    // 2. Migrate the previous workspace/aspect scene first, then legacy aspect scene.
    obs_source_t *sourceToClone = nullptr;
	bool donorHasDeclaredAspect = false;
	bool donorHasExactSize = false;
    if (!workspace_id.empty()) {
		std::string oldWorkspaceScene = logicalName + " [WS:" + workspace_id + "] [" + aspect_key + "]";
		sourceToClone = obs_get_source_by_name(oldWorkspaceScene.c_str());
		donorHasDeclaredAspect = sourceToClone != nullptr;
	}
	// Copy another workspace only for an explicit Duplicate Workspace operation.
	if (!sourceToClone && !templateWorkspaceId.empty()) {
		const std::string templateName = GetBackingSceneName(logicalName, aspect_key, templateWorkspaceId,
											 targetCX, targetCY);
		sourceToClone = obs_get_source_by_name(templateName.c_str());
		donorHasDeclaredAspect = sourceToClone != nullptr;
		donorHasExactSize = sourceToClone != nullptr;
	}
    if (!sourceToClone && !workspace_id.empty()) {
        std::string legacyAspectScene = logicalName + " [" + aspect_key + "]";
        sourceToClone = obs_get_source_by_name(legacyAspectScene.c_str());
		donorHasDeclaredAspect = sourceToClone != nullptr;
    }
	// Untagged templates are eligible only when their saved dimensions match.
	// A missing portrait design must never fall back to a landscape scene.
	if (!sourceToClone) {
		obs_source_t *legacy = obs_get_source_by_name(logicalName.c_str());
		if (legacy) {
			obs_data_t *settings = obs_source_get_settings(legacy);
			const bool matches = obs_scene_from_source(legacy) && settings &&
				obs_data_get_bool(settings, "custom_size") &&
				obs_data_get_int(settings, "cx") == targetCX &&
				obs_data_get_int(settings, "cy") == targetCY;
			if (settings) obs_data_release(settings);
			if (matches) {
				sourceToClone = legacy;
				donorHasDeclaredAspect = true;
				donorHasExactSize = true;
			} else {
				obs_source_release(legacy);
			}
		}
	}

    obs_scene_t *newScene = obs_scene_create(targetBacking.c_str());
    if (!newScene) {
        if (sourceToClone) obs_source_release(sourceToClone);
        blog(LOG_ERROR, "[Workspace] Could not create backing scene '%s'", targetBacking.c_str());
        return "";
    }

    // Set the scene's actual libobs dimensions before copying item transforms.
    obs_scene_set_custom_size(newScene, true, targetCX, targetCY);

    if (sourceToClone) {
        obs_scene_t *srcScene = obs_scene_from_source(sourceToClone);
        if (srcScene) {
            // Legacy scenes were attached to the changing main canvas. Pin their
            // declared design aspect before get_info2 reads pixel coordinates.
			if (donorHasDeclaredAspect) {
				uint32_t sourceCX = donorHasExactSize ? targetCX : 1920;
				uint32_t sourceCY = donorHasExactSize ? targetCY : 1080;
				if (!donorHasExactSize && aspect_key == "9:16") { sourceCX = 1080; sourceCY = 1920; }
				else if (!donorHasExactSize && aspect_key == "4:3") { sourceCX = 1440; sourceCY = 1080; }
				else if (!donorHasExactSize && aspect_key == "4:5") { sourceCX = 1080; sourceCY = 1350; }
				else if (!donorHasExactSize && aspect_key == "1:1") { sourceCX = 1080; sourceCY = 1080; }
				obs_scene_set_custom_size(srcScene, true, sourceCX, sourceCY);
			} else {
				obs_video_info currentVideo{};
				if (obs_get_video_info(&currentVideo))
					obs_scene_set_custom_size(srcScene, true, currentVideo.base_width, currentVideo.base_height);
			}

            CopySceneItems(newScene, srcScene);
        }
        obs_source_release(sourceToClone);
    }

    IsolateDesignSources(newScene, targetBacking);

    blog(LOG_INFO, "[Workspace] Created independent backing scene '%s' for aspect '%s' (%ux%u)",
         targetBacking.c_str(), aspect_key.c_str(), targetCX, targetCY);

    // The scene list keeps its own reference; drop the one from obs_scene_create
    // so the scene and its sources can be freed when scene data is cleared.
    obs_scene_release(newScene);

    return targetBacking;
}

void OBSWorkspaceManager::SaveSceneLayout(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key, const std::vector<SceneItemTransform> &items)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws || scene_name.empty() || aspect_key.empty()) return;
    ws->scene_layouts[scene_name][aspect_key] = items;
    Save();
}

bool OBSWorkspaceManager::GetSceneLayout(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key, std::vector<SceneItemTransform> &out_items) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            auto sc_it = ws.scene_layouts.find(scene_name);
            if (sc_it != ws.scene_layouts.end()) {
                auto asp_it = sc_it->second.find(aspect_key);
                if (asp_it != sc_it->second.end()) {
                    out_items = asp_it->second;
                    return true;
                }
            }
            break;
        }
    }
    return false;
}


// -------------------------------------------------------
// Save
// -------------------------------------------------------
bool OBSWorkspaceManager::Save(const std::string &path)
{
    std::string target_path = path.empty() ? (json_path_.empty() ? GetWorkspaceFilePath("workspace.json") : json_path_) : path;
    if (target_path.empty()) return false;
    std::string json_str = SerializeJson();
	if (path.empty() && canonical_project_loaded_ && !project_path_.empty()) {
		std::string error;
		const bool saved = HKProject::SaveProjectWorkspace(project_path_, json_path_, scene_collection_path_,
								   json_str, &error);
		if (!saved)
			blog(LOG_ERROR, "[HKProject] Could not save project %s: %s", project_path_.c_str(), error.c_str());
		return saved;
	}

    QFileInfo fi(QString::fromStdString(target_path));
    fi.dir().mkpath(".");
	BackupValidWorkspaceFile(fi.filePath());

    QSaveFile f(QString::fromStdString(target_path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        blog(LOG_ERROR, "[Workspaces] Could not write %s: %s", target_path.c_str(), qPrintable(f.errorString()));
        return false;
    }
    const QByteArray jsonBytes = QByteArray::fromStdString(json_str);
    if (f.write(jsonBytes) != jsonBytes.size() || !f.commit()) {
        blog(LOG_ERROR, "[Workspaces] Could not commit %s: %s", target_path.c_str(), qPrintable(f.errorString()));
        return false;
    }

    return true;
}

bool OBSWorkspaceManager::SaveSceneCollectionJson(const std::string &sceneCollectionJson,
						  const std::string &legacyPath)
{
	if (project_path_.empty() || sceneCollectionJson.empty())
		return false;
	if (!legacyPath.empty())
		scene_collection_path_ = legacyPath;
	std::string error;
	const bool saved = HKProject::SaveProjectSceneCollection(project_path_, json_path_, scene_collection_path_,
									  SerializeJson(), sceneCollectionJson, &error);
	if (!saved) {
		blog(LOG_ERROR, "[HKProject] Could not save scene collection into %s: %s", project_path_.c_str(),
		     error.c_str());
		return false;
	}
	canonical_project_loaded_ = true;
	if (!error.empty())
		blog(LOG_WARNING, "[HKProject] %s", error.c_str());
	return true;
}

std::string OBSWorkspaceManager::SerializeJson() const
{
    json_t *root = json_object();
    json_object_set_new(root, "version", json_integer(1));
    json_object_set_new(root, "active_workspace", json_string(active_id_.c_str()));

    json_t *arr = json_array();
    for (auto &ws : workspaces_) {
        json_t *ws_j = json_object();
        json_object_set_new(ws_j, "id",           json_string(ws.id.c_str()));
        json_object_set_new(ws_j, "name",         json_string(ws.name.c_str()));
        json_object_set_new(ws_j, "icon",         json_string(ws.icon.c_str()));
        json_object_set_new(ws_j, "active_scene", json_string(ws.active_scene.c_str()));
        json_object_set_new(ws_j, "interactive_mode_enabled", json_boolean(ws.interactive_mode_enabled));
        json_object_set_new(ws_j, "scenes",       vec_to_json_array(ws.scenes));
        if (!ws.hidden_scenes.empty()) {
            json_object_set_new(ws_j, "hidden_scenes", vec_to_json_array(ws.hidden_scenes));
        }
        if (!ws.hidden_scenes_by_aspect.empty()) {
            json_t *hba_j = json_object();
            for (const auto &pair : ws.hidden_scenes_by_aspect) {
                json_object_set_new(hba_j, pair.first.c_str(), vec_to_json_array(pair.second));
            }
            json_object_set_new(ws_j, "hidden_scenes_by_aspect", hba_j);
        }

        json_t *video_j = json_object();
        json_object_set_new(video_j, "base_cx",   json_integer(ws.video.base_cx));
        json_object_set_new(video_j, "base_cy",   json_integer(ws.video.base_cy));
        json_object_set_new(video_j, "output_cx", json_integer(ws.video.output_cx));
        json_object_set_new(video_j, "output_cy", json_integer(ws.video.output_cy));
        json_object_set_new(video_j, "fps_num",   json_integer(ws.video.fps_num));
        json_object_set_new(video_j, "fps_den",   json_integer(ws.video.fps_den));
        json_object_set_new(ws_j, "video", video_j);

        json_t *sources_j = json_object();
        json_object_set_new(sources_j, "core",     vec_to_json_array(ws.core_sources));
        json_object_set_new(sources_j, "variable", vec_to_json_array(ws.variable_sources));
        json_object_set_new(ws_j, "sources", sources_j);

        if (!ws.scene_resolutions.empty()) {
            json_t *scene_res_j = json_object();
            for (const auto &pair : ws.scene_resolutions) {
                json_t *res_obj = json_object();
                json_object_set_new(res_obj, "cx", json_integer(pair.second.first));
                json_object_set_new(res_obj, "cy", json_integer(pair.second.second));
                json_object_set_new(scene_res_j, pair.first.c_str(), res_obj);
            }
            json_object_set_new(ws_j, "scene_resolutions", scene_res_j);
        }

        if (!ws.scene_layouts.empty()) {
            json_t *layouts_j = json_object();
            for (const auto &scene_pair : ws.scene_layouts) {
                json_t *aspects_j = json_object();
                for (const auto &aspect_pair : scene_pair.second) {
                    json_t *items_j = json_array();
                    for (const auto &it : aspect_pair.second) {
                        json_t *item_j = json_object();
                        json_object_set_new(item_j, "name", json_string(it.name.c_str()));
                        json_object_set_new(item_j, "id", json_integer(it.id));
                        json_object_set_new(item_j, "pos_x", json_real(it.pos_x));
                        json_object_set_new(item_j, "pos_y", json_real(it.pos_y));
                        json_object_set_new(item_j, "scale_x", json_real(it.scale_x));
                        json_object_set_new(item_j, "scale_y", json_real(it.scale_y));
                        json_object_set_new(item_j, "rot", json_real(it.rot));
                        json_object_set_new(item_j, "alignment", json_integer(it.alignment));
                        json_object_set_new(item_j, "bounds_type", json_integer(it.bounds_type));
                        json_object_set_new(item_j, "bounds_alignment", json_integer(it.bounds_alignment));
                        json_object_set_new(item_j, "bounds_x", json_real(it.bounds_x));
                        json_object_set_new(item_j, "bounds_y", json_real(it.bounds_y));
                        json_object_set_new(item_j, "crop_to_bounds", json_boolean(it.crop_to_bounds));
                        json_object_set_new(item_j, "crop_left", json_integer(it.crop_left));
                        json_object_set_new(item_j, "crop_top", json_integer(it.crop_top));
                        json_object_set_new(item_j, "crop_right", json_integer(it.crop_right));
                        json_object_set_new(item_j, "crop_bottom", json_integer(it.crop_bottom));
                        json_object_set_new(item_j, "visible", json_boolean(it.visible));
                        json_array_append_new(items_j, item_j);
                    }
                    json_object_set_new(aspects_j, aspect_pair.first.c_str(), items_j);
                }
                json_object_set_new(layouts_j, scene_pair.first.c_str(), aspects_j);
            }
            json_object_set_new(ws_j, "scene_layouts", layouts_j);
        }

        if (!ws.base_scene.empty()) {
            json_object_set_new(ws_j, "base_scene", json_string(ws.base_scene.c_str()));
        }

		// Keep this map present even when empty so canonical-project saves can
		// distinguish "no interactive scenes" from "leave old roles untouched".
		json_t *inter_j = json_object();
		for (const auto &pair : ws.interactive_scenes) {
			json_t *config_j = json_object();
			json_object_set_new(config_j, "source", json_string(pair.second.source_name.c_str()));
			json_object_set_new(config_j, "fallback", json_string(pair.second.fallback_scene.c_str()));
			json_object_set_new(config_j, "enabled", json_boolean(pair.second.enabled));
			json_object_set_new(inter_j, pair.first.c_str(), config_j);
        }
		json_object_set_new(ws_j, "interactive_scenes", inter_j);

        if (!ws.scene_text_colors.empty()) {
            json_t *colors_j = json_object();
            for (const auto &sc_pair : ws.scene_text_colors) {
                json_t *elems_j = json_object();
                for (const auto &elem_pair : sc_pair.second) {
                    json_object_set_new(elems_j, elem_pair.first.c_str(), json_string(elem_pair.second.c_str()));
                }
                json_object_set_new(colors_j, sc_pair.first.c_str(), elems_j);
            }
            json_object_set_new(ws_j, "scene_text_colors", colors_j);
        }

        json_array_append_new(arr, ws_j);
    }
    json_object_set_new(root, "workspaces", arr);

    char *dump = json_dumps(root, JSON_INDENT(2) | JSON_ENSURE_ASCII);
    std::string result(dump ? dump : "{}");
    free(dump);
    json_decref(root);
    return result;
}

// -------------------------------------------------------
// Accessors
// -------------------------------------------------------
const OBSWorkspace *OBSWorkspaceManager::GetActive() const
{
    for (auto &ws : workspaces_)
        if (ws.id == active_id_)
            return &ws;
    return workspaces_.empty() ? nullptr : &workspaces_[0];
}

OBSWorkspace *OBSWorkspaceManager::GetById(const std::string &id)
{
    for (auto &ws : workspaces_)
        if (ws.id == id)
            return &ws;
    return nullptr;
}

void OBSWorkspaceManager::SetActiveScene(const std::string &ws_id, const std::string &scene_name)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (ws)
        ws->active_scene = scene_name;
}

// -------------------------------------------------------
// SwitchTo — apply video settings and activate scene
// -------------------------------------------------------
bool OBSWorkspaceManager::SwitchTo(const std::string &id)
{
    OBSWorkspace *ws = GetById(id);
    if (!ws) {
        blog(LOG_WARNING, "[Workspaces] SwitchTo: id not found: %s", id.c_str());
        return false;
    }

    // Save current active scene ONLY if it actually belongs to the current workspace
    if (OBSWorkspace *cur = GetById(active_id_)) {
        obs_source_t *cur_scene = obs_frontend_get_current_scene();
        if (cur_scene) {
            std::string cur_name = obs_source_get_name(cur_scene);
            if (std::find(cur->scenes.begin(), cur->scenes.end(), cur_name) != cur->scenes.end()) {
                cur->active_scene = cur_name;
            }
            obs_source_release(cur_scene);
        }
    }

    active_id_ = id;

    // Apply video configuration
    obs_video_info ovi = {};
    obs_get_video_info(&ovi);
    ovi.base_width    = ws->video.base_cx;
    ovi.base_height   = ws->video.base_cy;
    ovi.output_width  = ws->video.output_cx;
    ovi.output_height = ws->video.output_cy;
    ovi.fps_num = ws->video.fps_num;
    ovi.fps_den = ws->video.fps_den;
    int ret = obs_reset_video(&ovi);
    if (ret != OBS_VIDEO_SUCCESS) {
        blog(LOG_WARNING, "[Workspaces] obs_reset_video failed: %d", ret);
    }

    // Find and switch to the correct target scene for this workspace
    obs_source_t *target_scene_src = nullptr;
    if (!ws->active_scene.empty()) {
        if (std::find(ws->scenes.begin(), ws->scenes.end(), ws->active_scene) != ws->scenes.end()) {
            target_scene_src = obs_get_source_by_name(ws->active_scene.c_str());
        }
    }

    if (!target_scene_src) {
        for (const auto &s : ws->scenes) {
            target_scene_src = obs_get_source_by_name(s.c_str());
            if (target_scene_src) {
                ws->active_scene = s;
                break;
            }
        }
    }

    if (!target_scene_src) {
        target_scene_src = obs_get_source_by_name(ws->name.c_str());
        if (target_scene_src) {
            ws->active_scene = ws->name;
            if (std::find(ws->scenes.begin(), ws->scenes.end(), ws->name) == ws->scenes.end()) {
                ws->scenes.push_back(ws->name);
            }
        }
    }

    if (target_scene_src) {
        obs_frontend_set_current_scene(target_scene_src);
        obs_source_release(target_scene_src);
    }

    Save();
    blog(LOG_INFO, "[Workspaces] Switched to: %s (%s)", ws->name.c_str(), ws->id.c_str());
    return true;
}

void OBSWorkspaceManager::SetBaseScene(const std::string &ws_id, const std::string &scene_name)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return;
    ws->base_scene = scene_name;
    // The designated base scene is always fixed.
    ws->interactive_scenes.erase(scene_name);
    for (auto &pair : ws->interactive_scenes) {
        if (pair.second.fallback_scene.empty())
            pair.second.fallback_scene = scene_name;
    }
    Save();
}

std::string OBSWorkspaceManager::GetBaseScene(const std::string &ws_id) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) return ws.base_scene;
    }
    return "";
}

void OBSWorkspaceManager::SetInteractiveScene(const std::string &ws_id, const std::string &scene_name,
                                              const std::string &source_name, const std::string &fallback_scene)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws || scene_name.empty() || source_name.empty() || fallback_scene.empty() ||
        scene_name == fallback_scene || scene_name == ws->base_scene)
        return;
    if (GetSourceCategory(ws_id, source_name) != 1)
        return;
    if (!FindConflictSceneForSource(ws_id, source_name, scene_name).empty())
        return;
    if (std::find(ws->scenes.begin(), ws->scenes.end(), scene_name) == ws->scenes.end() ||
        std::find(ws->scenes.begin(), ws->scenes.end(), fallback_scene) == ws->scenes.end())
        return;
    auto fallbackInteractive = ws->interactive_scenes.find(fallback_scene);
    if (fallbackInteractive != ws->interactive_scenes.end())
        return; // An alternative must be a fixed scene to prevent recursive switching.

	bool enabled = true;
	auto previous = ws->interactive_scenes.find(scene_name);
	const bool hadPrevious = previous != ws->interactive_scenes.end();
	OBSInteractiveSceneConfig previousConfig;
	if (hadPrevious) {
		previousConfig = previous->second;
		enabled = previous->second.enabled;
	}
	ws->interactive_scenes[scene_name] = {source_name, fallback_scene, enabled};
	if (!Save()) {
		if (hadPrevious)
			ws->interactive_scenes[scene_name] = std::move(previousConfig);
		else
			ws->interactive_scenes.erase(scene_name);
	}
}

bool OBSWorkspaceManager::SetInteractiveSceneEnabled(const std::string &ws_id, const std::string &scene_name,
                                                     bool enabled)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return false;
    auto it = ws->interactive_scenes.find(scene_name);
    if (it == ws->interactive_scenes.end()) return false;
    if (it->second.enabled == enabled) return true;
	const bool previousEnabled = it->second.enabled;
    it->second.enabled = enabled;
	if (Save())
		return true;
	it->second.enabled = previousEnabled;
	return false;
}

bool OBSWorkspaceManager::IsInteractiveSceneEnabled(const std::string &ws_id, const std::string &scene_name) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id != ws_id) continue;
        const auto it = ws.interactive_scenes.find(scene_name);
        return it != ws.interactive_scenes.end() && it->second.enabled;
    }
    return false;
}

std::string OBSWorkspaceManager::GetInteractiveSource(const std::string &ws_id, const std::string &scene_name) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            auto it = ws.interactive_scenes.find(scene_name);
            if (it != ws.interactive_scenes.end()) return it->second.source_name;
            break;
        }
    }
    return "";
}

std::string OBSWorkspaceManager::GetInteractiveFallbackScene(const std::string &ws_id,
                                                              const std::string &scene_name) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            auto it = ws.interactive_scenes.find(scene_name);
            if (it != ws.interactive_scenes.end()) {
                return it->second.fallback_scene.empty() ? ws.base_scene : it->second.fallback_scene;
            }
            break;
        }
    }
    return "";
}

bool OBSWorkspaceManager::ClearSceneRole(const std::string &ws_id, const std::string &scene_name)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return false;
	const std::string previousBaseScene = ws->base_scene;
	auto previousInteractive = ws->interactive_scenes.find(scene_name);
	const bool hadInteractive = previousInteractive != ws->interactive_scenes.end();
	OBSInteractiveSceneConfig previousConfig;
	if (hadInteractive)
		previousConfig = previousInteractive->second;

    if (ws->base_scene == scene_name) {
        ws->base_scene.clear();
    }
    ws->interactive_scenes.erase(scene_name);
	if (Save())
		return true;

	ws->base_scene = previousBaseScene;
	if (hadInteractive)
		ws->interactive_scenes[scene_name] = std::move(previousConfig);
	return false;
}

bool OBSWorkspaceManager::IsBaseScene(const std::string &ws_id, const std::string &scene_name) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) return ws.base_scene == scene_name;
    }
    return false;
}

bool OBSWorkspaceManager::IsInteractiveScene(const std::string &ws_id, const std::string &scene_name) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) return ws.interactive_scenes.find(scene_name) != ws.interactive_scenes.end();
    }
    return false;
}

std::string OBSWorkspaceManager::FindConflictSceneForSource(const std::string &ws_id, const std::string &source_name, const std::string &current_scene) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            for (const auto &pair : ws.interactive_scenes) {
                if (pair.first != current_scene && pair.second.source_name == source_name) {
                    return pair.first;
                }
            }
            break;
        }
    }
    return "";
}

bool OBSWorkspaceManager::DuplicateWorkspace(const std::string &sourceId, const std::string &newName, std::string &outNewId)
{
    const OBSWorkspace *src = GetById(sourceId);
    if (!src) src = GetActive();
    if (!src) return false;

    OBSWorkspace copy = *src;
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
    copy.id = src->id + "_copy_" + std::to_string(ts);
    copy.name = newName.empty() ? (src->name + " - نسخة") : newName;
    outNewId = copy.id;

	// Materialize copies from this workspace's own saved resolutions. Normal
	// scene activation never searches other workspaces for a design to copy.
	const std::string sourceWorkspaceId = src->id;
	const std::pair<uint32_t, uint32_t> sizes[] = {{1920, 1080}, {1080, 1920}, {1080, 1080}};
	for (const auto &sceneName : copy.scenes) {
		for (const auto &size : sizes) {
			const std::string aspect = GetAspectRatioKey(size.first, size.second);
			const std::string sourceName = GetBackingSceneName(sceneName, aspect, sourceWorkspaceId,
											 size.first, size.second);
			obs_source_t *existing = obs_get_source_by_name(sourceName.c_str());
			if (!existing) continue;
			obs_source_release(existing);
			GetOrCreateBackingScene(sceneName, aspect, copy.id, size.first, size.second, sourceWorkspaceId);
		}
	}

    workspaces_.push_back(copy);
    Save();
    return true;
}

bool OBSWorkspaceManager::ToggleSceneHidden(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return false;

    std::string key = aspect_key.empty() ? GetAspectRatioKey(ws->video.base_cx, ws->video.base_cy) : aspect_key;
    auto &list = ws->hidden_scenes_by_aspect[key];

    auto it = std::find(list.begin(), list.end(), scene_name);
    if (it != list.end()) {
        list.erase(it);
        Save();
        return false; // Now visible in this aspect ratio
    } else {
        list.push_back(scene_name);
        Save();
        return true; // Now hidden in this aspect ratio
    }
}

bool OBSWorkspaceManager::IsSceneHidden(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            std::string key = aspect_key.empty() ? GetAspectRatioKey(ws.video.base_cx, ws.video.base_cy) : aspect_key;
            auto it = ws.hidden_scenes_by_aspect.find(key);
            if (it != ws.hidden_scenes_by_aspect.end()) {
                return std::find(it->second.begin(), it->second.end(), scene_name) != it->second.end();
            }
            return std::find(ws.hidden_scenes.begin(), ws.hidden_scenes.end(), scene_name) != ws.hidden_scenes.end();
        }
    }
    return false;
}

void OBSWorkspaceManager::SetSceneTextColor(const std::string &ws_id, const std::string &scene_name, const std::string &element_id, const std::string &color_hex)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return;
    const std::string storageKey = scene_name + " [" + ResolutionKey(ws->video.base_cx, ws->video.base_cy) + "]";
    if (color_hex.empty()) {
        ws->scene_text_colors[storageKey].erase(element_id);
    } else {
        ws->scene_text_colors[storageKey][element_id] = color_hex;
    }
    Save();
}

std::string OBSWorkspaceManager::GetSceneTextColor(const std::string &ws_id, const std::string &scene_name, const std::string &element_id) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            const std::string storageKey = scene_name + " [" + ResolutionKey(ws.video.base_cx, ws.video.base_cy) + "]";
            auto sc_it = ws.scene_text_colors.find(storageKey);
            if (sc_it != ws.scene_text_colors.end()) {
                auto el_it = sc_it->second.find(element_id);
                if (el_it != sc_it->second.end()) {
                    return el_it->second;
                }
            }
            break;
        }
    }
    return "";
}

std::map<std::string, std::string> OBSWorkspaceManager::GetSceneTextColors(const std::string &ws_id, const std::string &scene_name) const
{
    for (const auto &ws : workspaces_) {
        if (ws.id == ws_id) {
            const std::string storageKey = scene_name + " [" + ResolutionKey(ws.video.base_cx, ws.video.base_cy) + "]";
            auto sc_it = ws.scene_text_colors.find(storageKey);
            if (sc_it != ws.scene_text_colors.end()) {
                return sc_it->second;
            }
            break;
        }
    }
    return {};
}

void OBSWorkspaceManager::SetSceneTextColors(const std::string &ws_id, const std::string &scene_name, const std::map<std::string, std::string> &colors)
{
    OBSWorkspace *ws = GetById(ws_id);
    if (!ws) return;
    const std::string storageKey = scene_name + " [" + ResolutionKey(ws->video.base_cx, ws->video.base_cy) + "]";
    ws->scene_text_colors[storageKey] = colors;
    Save();
}

void OBSWorkspaceManager::ApplySceneTextColors(const std::string &ws_id, const std::string &scene_name)
{
    const OBSWorkspace *ws = GetById(ws_id);
    if (!ws || scene_name.empty()) return;

    auto colors = GetSceneTextColors(ws_id, scene_name);

    std::string aspect_key = GetAspectRatioKey(ws->video.base_cx, ws->video.base_cy);
    std::string backingName = GetOrCreateBackingScene(scene_name, aspect_key, ws_id,
											  ws->video.base_cx, ws->video.base_cy);
    obs_source_t *sceneSrc = obs_get_source_by_name(backingName.c_str());
    if (!sceneSrc) return;

    obs_scene_t *scene = obs_scene_from_source(sceneSrc);
    if (!scene) {
        obs_source_release(sceneSrc);
        return;
    }

    struct TextElemDef {
        std::string id;
        std::vector<std::string> keys;
    };
    std::vector<TextElemDef> elemDefs = {
        {"العنوان", {"العنوان", "عنوان الدرس", "عنوان"}},
        {"تاريخ", {"تاريخ", "التاريخ"}},
        {"تنبيه الوقت", {"تنبيه الوقت", "تنبيه وقت", "تنبيه"}},
        {"عداد الوقت", {"عداد الوقت", "العداد", "عداد"}}
    };

    struct ApplyData {
        const std::map<std::string, std::string> *colors;
        const std::vector<TextElemDef> *defs;
    } data = {&colors, &elemDefs};

    obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *p) -> bool {
        auto *d = static_cast<ApplyData *>(p);
        obs_source_t *src = obs_sceneitem_get_source(item);
        if (!src) return true;
        const char *srcName = obs_source_get_name(src);
        if (!srcName) return true;
        std::string nameStr(srcName);

        for (const auto &def : *(d->defs)) {
            bool matches = false;
            for (const auto &k : def.keys) {
                if (nameStr.find(k) != std::string::npos) {
                    matches = true;
                    break;
                }
            }
            if (matches) {
                obs_data_t *settings = obs_source_get_settings(src);
                const std::string previousSettings = obs_data_get_json(settings);

                // Preserve the source's baseline while changing only its color.
                obs_data_t *priv = obs_source_get_private_settings(src);
                const char *srcId = obs_source_get_id(src);

                auto colIt = d->colors->find(def.id);
                bool hasCustom = (colIt != d->colors->end() && !colIt->second.empty());
                if (!hasCustom && (!priv || !obs_data_get_bool(priv, "hk_has_orig_baseline"))) {
                    if (priv) obs_data_release(priv);
                    obs_data_release(settings);
                    break;
                }

                if (priv) {
                    if (!obs_data_get_bool(priv, "hk_has_orig_baseline")) {
                        obs_data_set_bool(priv, "hk_has_orig_baseline", true);
						if (IsBrowserSourceId(srcId)) {
                            const char *curCss = obs_data_get_string(settings, "css");
                            obs_data_set_string(priv, "hk_orig_css", curCss ? curCss : "");
                        } else if (srcId && strcmp(srcId, "text_ft2_source") == 0) {
                            obs_data_set_int(priv, "hk_orig_color1", obs_data_get_int(settings, "color1"));
                            obs_data_set_int(priv, "hk_orig_color2", obs_data_get_int(settings, "color2"));
                        } else {
                            int64_t curCol = obs_data_get_int(settings, "color");
                            obs_data_set_int(priv, "hk_orig_color", curCol != 0 ? curCol : 0xFFFFFFFF);
                        }
                    }
                }

				if (IsBrowserSourceId(srcId)) {
                    if (hasCustom) {
                        std::string hex = colIt->second;
                        std::string customCss = "body, body *, #date, div, span, p { color: " + hex + " !important; }";
                        obs_data_set_string(settings, "css", customCss.c_str());
                    } else {
                        // Restore exact original baseline CSS
                        const char *origCss = priv ? obs_data_get_string(priv, "hk_orig_css") : "";
                        if (origCss && *origCss) {
                            obs_data_set_string(settings, "css", origCss);
                        } else {
                            obs_data_set_string(settings, "css", "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }");
                        }
                    }
                } else if (srcId && strcmp(srcId, "text_ft2_source") == 0) {
                    if (hasCustom) {
                        QColor col(QString::fromStdString(colIt->second));
                        if (col.isValid()) {
                            uint32_t val = ((unsigned)(col.red()) & 0xff) |
                                           (((unsigned)(col.green()) & 0xff) << 8) |
                                           (((unsigned)(col.blue()) & 0xff) << 16) |
                                           (((unsigned)(col.alpha()) & 0xff) << 24);
                            obs_data_set_int(settings, "color1", val);
                            obs_data_set_int(settings, "color2", val);
                        }
                    } else {
                        // Restore exact original baseline colors
                        int64_t c1 = priv ? obs_data_get_int(priv, "hk_orig_color1") : 0xFFFFFFFF;
                        int64_t c2 = priv ? obs_data_get_int(priv, "hk_orig_color2") : 0xFFFFFFFF;
                        obs_data_set_int(settings, "color1", c1);
                        obs_data_set_int(settings, "color2", c2);
                    }
                } else {
                    // text_gdiplus or generic text source
                    if (hasCustom) {
                        QColor col(QString::fromStdString(colIt->second));
                        if (col.isValid()) {
                            uint32_t val = ((unsigned)(col.red()) & 0xff) |
                                           (((unsigned)(col.green()) & 0xff) << 8) |
                                           (((unsigned)(col.blue()) & 0xff) << 16) |
                                           (((unsigned)(col.alpha()) & 0xff) << 24);
                            obs_data_set_int(settings, "color", val);
                        }
                    } else {
                        // Restore exact original baseline color
                        int64_t origC = priv ? obs_data_get_int(priv, "hk_orig_color") : 0xFFFFFFFF;
                        obs_data_set_int(settings, "color", origC != 0 ? origC : 0xFFFFFFFF);
                    }
                }

                // Role changes must not resize sources or reload unchanged pages.
                if (previousSettings != obs_data_get_json(settings))
                    obs_source_update(src, settings);

                if (priv) {
                    obs_data_release(priv);
                }
                obs_data_release(settings);
                break;
            }
        }
        return true;
    }, &data);

    obs_source_release(sceneSrc);
}

#include "HKProjectStore.hpp"

#include <jansson.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QUuid>

#include <unordered_set>
#include <vector>
#include <cstring>
#include <cstdlib>

namespace HKProject {
namespace {

json_t *LoadJson(const std::string &path, std::string *error)
{
	json_error_t jsonError{};
	json_t *root = json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &jsonError);
	if (!root && error) {
		*error = "Could not parse " + path + " at line " + std::to_string(jsonError.line) + ": " +
			 std::string(jsonError.text);
	}
	return root;
}

bool SaveJsonAtomic(json_t *root, const std::string &path, std::string *error)
{
	if (!root || path.empty()) {
		if (error)
			*error = "Project output path or document is empty";
		return false;
	}

	QFileInfo info(QString::fromUtf8(path.c_str()));
	if (!info.dir().mkpath(".")) {
		if (error)
			*error = "Could not create project directory: " + info.dir().absolutePath().toUtf8().toStdString();
		return false;
	}

	char *serialized = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
	if (!serialized) {
		if (error)
			*error = "Could not serialize project document";
		return false;
	}

	QSaveFile output(info.absoluteFilePath());
	const QByteArray bytes(serialized);
	free(serialized);
	QFile previous(info.absoluteFilePath());
	if (previous.exists() && previous.open(QIODevice::ReadOnly)) {
		const QByteArray previousBytes = previous.readAll();
		json_error_t previousError{};
		json_t *previousJson = json_loadb(previousBytes.constData(), static_cast<size_t>(previousBytes.size()),
						  JSON_REJECT_DUPLICATES, &previousError);
		previous.close();
		if (previousJson) {
			json_decref(previousJson);
			QSaveFile backup(info.absoluteFilePath() + ".bak");
			if (backup.open(QIODevice::WriteOnly) && backup.write(previousBytes) == previousBytes.size())
				backup.commit();
		}
	}
	if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
		if (error)
			*error = "Could not commit project file: " + output.errorString().toUtf8().toStdString();
		return false;
	}
	return true;
}

bool IsProjectV2(json_t *project)
{
	const char *format = json_is_object(project) ? json_string_value(json_object_get(project, "format")) : nullptr;
	json_t *version = json_is_object(project) ? json_object_get(project, "schema_version") : nullptr;
	return format && strcmp(format, "HKProject") == 0 && json_is_integer(version) &&
	       json_integer_value(version) == 2 && json_is_object(json_object_get(project, "workspaces")) &&
	       json_is_array(json_object_get(project, "resources")) &&
	       json_is_array(json_object_get(project, "scenes")) &&
	       json_is_array(json_object_get(project, "groups")) &&
	       json_is_array(json_object_get(project, "source_order"));
}

json_t *LoadProjectWithRecovery(const std::string &path, std::string *error)
{
	std::string currentError;
	json_t *project = LoadJson(path, &currentError);
	if (IsProjectV2(project)) {
		if (error)
			error->clear();
		return project;
	}
	if (project) {
		json_t *version = json_object_get(project, "schema_version");
		if (json_is_integer(version) && json_integer_value(version) > 2) {
			if (error)
				*error = "HKProject schema is newer than this application supports";
			json_decref(project);
			return nullptr;
		}
		json_decref(project);
		currentError = "HKProject v2 is missing required sections";
	}

	json_t *backup = LoadJson(path + ".bak", nullptr);
	if (IsProjectV2(backup)) {
		const QString projectPath = QString::fromUtf8(path.c_str());
		if (QFileInfo::exists(projectPath)) {
			QString corruptPath = projectPath + ".corrupt";
			for (unsigned int index = 1; QFileInfo::exists(corruptPath); ++index)
				corruptPath = projectPath + QStringLiteral(".corrupt.%1").arg(index);
			QFile::copy(projectPath, corruptPath);
		}
		std::string recoveryError;
		if (!SaveJsonAtomic(backup, path, &recoveryError) && error)
			*error = "Loaded the project backup, but could not restore the primary file: " + recoveryError;
		else if (error)
			error->clear();
		return backup;
	}
	if (backup)
		json_decref(backup);
	if (error)
		*error = currentError.empty() ? "HKProject v2 and its backup could not be loaded" : currentError;
	return nullptr;
}

std::string SourceKey(json_t *source, size_t index)
{
	const char *uuid = json_string_value(json_object_get(source, "uuid"));
	if (uuid && *uuid)
		return uuid;

	const char *id = json_string_value(json_object_get(source, "id"));
	const char *name = json_string_value(json_object_get(source, "name"));
	return std::string("legacy:") + (id ? id : "unknown") + ":" + (name ? name : "unnamed") + ":" +
	       std::to_string(index);
}

json_t *MakeSourceRecord(json_t *source, const std::string &key)
{
	json_t *record = json_object();
	json_object_set_new(record, "key", json_string(key.c_str()));
	json_object_set(record, "definition", source);
	return record;
}

json_t *FindDefinition(json_t *records, const char *key)
{
	if (!json_is_array(records) || !key)
		return nullptr;

	size_t index;
	json_t *record;
	json_array_foreach(records, index, record)
	{
		const char *candidate = json_string_value(json_object_get(record, "key"));
		if (candidate && strcmp(candidate, key) == 0)
			return json_object_get(record, "definition");
	}
	return nullptr;
}

void CountSceneItems(json_t *scenes, MigrationStats *stats)
{
	if (!stats || !json_is_array(scenes))
		return;

	size_t sceneIndex;
	json_t *record;
	json_array_foreach(scenes, sceneIndex, record)
	{
		json_t *definition = json_object_get(record, "definition");
		json_t *settings = json_object_get(definition, "settings");
		json_t *items = json_object_get(settings, "items");
		if (json_is_array(items))
			stats->sceneItems += json_array_size(items);
	}
}

json_t *CollectUnresolvedReferences(json_t *scenes, const std::unordered_set<std::string> &sourceKeys,
				    size_t *count)
{
	json_t *unresolved = json_array();
	if (count)
		*count = 0;
	if (!json_is_array(scenes))
		return unresolved;

	size_t sceneIndex;
	json_t *sceneRecord;
	json_array_foreach(scenes, sceneIndex, sceneRecord)
	{
		json_t *definition = json_object_get(sceneRecord, "definition");
		json_t *settings = json_object_get(definition, "settings");
		json_t *items = json_object_get(settings, "items");
		if (!json_is_array(items))
			continue;

		const char *sceneKey = json_string_value(json_object_get(sceneRecord, "key"));
		const char *sceneName = json_string_value(json_object_get(definition, "name"));
		size_t itemIndex;
		json_t *item;
		json_array_foreach(items, itemIndex, item)
		{
			const char *sourceUuid = json_string_value(json_object_get(item, "source_uuid"));
			if (!sourceUuid || sourceKeys.find(sourceUuid) != sourceKeys.end())
				continue;

			json_t *reference = json_object();
			json_object_set_new(reference, "scene_key", json_string(sceneKey ? sceneKey : ""));
			json_object_set_new(reference, "scene_name", json_string(sceneName ? sceneName : ""));
			if (json_t *itemId = json_object_get(item, "id"))
				json_object_set(reference, "item_id", itemId);
			if (json_t *itemName = json_object_get(item, "name"))
				json_object_set(reference, "item_name", itemName);
			json_object_set_new(reference, "source_uuid", json_string(sourceUuid));
			json_array_append_new(unresolved, reference);
			if (count)
				++*count;
		}
	}
	return unresolved;
}

json_t *BuildProjectDocument(json_t *workspace, json_t *sceneCollection, MigrationStats *stats,
			     std::string *error)
{
	json_t *workspaces = json_object_get(workspace, "workspaces");
	json_t *sourceArray = json_object_get(sceneCollection, "sources");
	json_t *groupArray = json_object_get(sceneCollection, "groups");
	if (!json_is_object(workspace) || !json_is_array(workspaces) || !json_is_object(sceneCollection) ||
	    !json_is_array(sourceArray) || !json_is_array(groupArray)) {
		if (error)
			*error = "Legacy workspace or OBS scene collection is missing required data";
		return nullptr;
	}

	json_t *project = json_object();
	json_t *resources = json_array();
	json_t *scenes = json_array();
	json_t *sourceOrder = json_array();
	std::unordered_set<std::string> keys;
	bool valid = true;
	size_t index;
	json_t *source;
	json_array_foreach(sourceArray, index, source)
	{
		if (!json_is_object(source)) {
			valid = false;
			break;
		}
		const std::string key = SourceKey(source, index);
		if (!keys.insert(key).second) {
			if (error)
				*error = "Duplicate source UUID or fallback identity in scene collection: " + key;
			valid = false;
			break;
		}
		json_t *record = MakeSourceRecord(source, key);
		const char *id = json_string_value(json_object_get(source, "id"));
		json_array_append_new(id && strcmp(id, "scene") == 0 ? scenes : resources, record);
		json_array_append_new(sourceOrder, json_string(key.c_str()));
	}
	if (!valid) {
		if (error && error->empty())
			*error = "OBS scene collection contains an invalid source record";
		json_decref(resources);
		json_decref(scenes);
		json_decref(sourceOrder);
		json_decref(project);
		return nullptr;
	}

	json_t *metadata = json_deep_copy(sceneCollection);
	json_object_del(metadata, "sources");
	json_object_del(metadata, "groups");
	const char *activeWorkspace = json_string_value(json_object_get(workspace, "active_workspace"));
	json_t *workspaceVersion = json_object_get(workspace, "version");
	const json_int_t fromWorkspaceVersion =
		json_is_integer(workspaceVersion) ? json_integer_value(workspaceVersion) : 1;
	const QByteArray projectId = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
	json_object_set_new(project, "format", json_string("HKProject"));
	json_object_set_new(project, "schema_version", json_integer(2));
	json_object_set_new(project, "project_id", json_string(projectId.constData()));
	json_object_set_new(project, "active_workspace_id", json_string(activeWorkspace ? activeWorkspace : ""));
	json_object_set(project, "workspaces", workspace);
	json_object_set_new(project, "scene_collection", metadata);
	json_object_set_new(project, "resources", resources);
	json_object_set_new(project, "scenes", scenes);
	json_object_set(project, "groups", groupArray);
	json_object_set_new(project, "source_order", sourceOrder);

	json_t *migration = json_object();
	json_object_set_new(migration, "from_workspace_schema", json_integer(fromWorkspaceVersion));
	const char *collectionName = json_string_value(json_object_get(sceneCollection, "name"));
	json_object_set_new(migration, "source_collection_name", json_string(collectionName ? collectionName : ""));
	json_t *unresolvedReferences = CollectUnresolvedReferences(scenes, keys,
									   stats ? &stats->unresolvedSourceReferences : nullptr);
	json_object_set_new(migration, "unresolved_source_references", unresolvedReferences);
	json_object_set_new(project, "migration", migration);

	if (stats) {
		stats->workspaces = json_array_size(workspaces);
		stats->resources = json_array_size(resources);
		stats->scenes = json_array_size(scenes);
		stats->groups = json_array_size(groupArray);
		CountSceneItems(scenes, stats);
	}
	return project;
}

json_t *MergePreservingUnknown(json_t *existing, json_t *incoming)
{
	if (json_is_object(existing) && json_is_object(incoming)) {
		json_t *merged = json_deep_copy(existing);
		const char *key;
		json_t *value;
		json_object_foreach(incoming, key, value)
		{
			// Scene roles are a complete user-managed map. Replacing the map is
			// necessary so removing the last interactive scene also removes its
			// previously saved entry from the canonical project.
			json_t *replacement = strcmp(key, "interactive_scenes") == 0
				? json_deep_copy(value)
				: MergePreservingUnknown(json_object_get(merged, key), value);
			if (replacement)
				json_object_set_new(merged, key, replacement);
		}
		return merged;
	}
	if (json_is_array(existing) && json_is_array(incoming)) {
		bool incomingHasIds = json_array_size(incoming) > 0;
		size_t index;
		json_t *entry;
		json_array_foreach(incoming, index, entry)
			incomingHasIds = incomingHasIds && json_is_object(entry) && json_is_string(json_object_get(entry, "id"));
		if (incomingHasIds) {
			json_t *merged = json_array();
			json_array_foreach(incoming, index, entry)
			{
				const char *id = json_string_value(json_object_get(entry, "id"));
				json_t *previous = nullptr;
				size_t oldIndex;
				json_t *oldEntry;
				json_array_foreach(existing, oldIndex, oldEntry)
				{
					const char *oldId = json_string_value(json_object_get(oldEntry, "id"));
					if (oldId && id && strcmp(oldId, id) == 0) {
						previous = oldEntry;
						break;
					}
				}
				json_t *mergedEntry = previous ? MergePreservingUnknown(previous, entry) : json_deep_copy(entry);
				json_array_append_new(merged, mergedEntry);
			}
			return merged;
		}
	}
	return incoming ? json_deep_copy(incoming) : nullptr;
}

bool ReplaceProjectSceneCollection(json_t *project, json_t *sceneCollection, std::string *error)
{
	json_t *workspace = json_object_get(project, "workspaces");
	if (!json_is_object(workspace)) {
		if (error)
			*error = "HKProject v2 has no workspace document";
		return false;
	}
	json_t *normalized = BuildProjectDocument(workspace, sceneCollection, nullptr, error);
	if (!normalized)
		return false;
	constexpr const char *collectionKeys[] = {"scene_collection", "resources", "scenes", "groups", "source_order"};
	for (const char *key : collectionKeys)
		json_object_set(project, key, json_object_get(normalized, key));
	json_object_set(project, "migration", json_object_get(normalized, "migration"));
	json_decref(normalized);
	return true;
}

json_t *BuildLegacySceneCollection(json_t *project, std::string *error)
{
	json_t *metadata = json_object_get(project, "scene_collection");
	json_t *resources = json_object_get(project, "resources");
	json_t *scenes = json_object_get(project, "scenes");
	json_t *groups = json_object_get(project, "groups");
	json_t *order = json_object_get(project, "source_order");
	if (!json_is_object(metadata) || !json_is_array(resources) || !json_is_array(scenes) ||
	    !json_is_array(groups) || !json_is_array(order)) {
		if (error)
			*error = "HKProject v2 is missing its scene collection, source graph, or ordering data";
		return nullptr;
	}

	json_t *output = json_deep_copy(metadata);
	json_t *sourceArray = json_array();
	size_t index;
	json_t *keyValue;
	json_array_foreach(order, index, keyValue)
	{
		const char *key = json_string_value(keyValue);
		json_t *definition = FindDefinition(scenes, key);
		if (!definition)
			definition = FindDefinition(resources, key);
		if (!definition) {
			if (error)
				*error = std::string("Source definition missing for key: ") + (key ? key : "(invalid)");
			json_decref(sourceArray);
			json_decref(output);
			return nullptr;
		}
		json_array_append(sourceArray, definition);
	}

	json_object_set_new(output, "sources", sourceArray);
	json_object_set(output, "groups", groups);
	return output;
}

} // namespace

bool MigrateLegacyProject(const std::string &workspaceFile, const std::string &sceneCollectionFile,
			  const std::string &outputFile, MigrationStats *stats, std::string *error)
{
	if (stats)
		*stats = {};
	if (error)
		error->clear();
	const QString outputPath = QFileInfo(QString::fromUtf8(outputFile.c_str())).absoluteFilePath();
	const QString workspacePath = QFileInfo(QString::fromUtf8(workspaceFile.c_str())).absoluteFilePath();
	const QString collectionPath = QFileInfo(QString::fromUtf8(sceneCollectionFile.c_str())).absoluteFilePath();
	if (outputPath.compare(workspacePath, Qt::CaseInsensitive) == 0 ||
	    outputPath.compare(collectionPath, Qt::CaseInsensitive) == 0) {
		if (error)
			*error = "HKProject output must be separate from both legacy input files";
		return false;
	}

	json_t *workspace = LoadJson(workspaceFile, error);
	if (!workspace)
		return false;
	json_t *sceneCollection = LoadJson(sceneCollectionFile, error);
	if (!sceneCollection) {
		json_decref(workspace);
		return false;
	}
	MigrationStats localStats;
	json_t *project = BuildProjectDocument(workspace, sceneCollection, &localStats, error);
	const bool saved = project && SaveJsonAtomic(project, outputFile, error);
	if (stats)
		*stats = localStats;
	if (project)
		json_decref(project);
	json_decref(workspace);
	json_decref(sceneCollection);
	return saved;
}

bool ExportLegacySceneCollection(const std::string &projectFile, const std::string &outputFile, std::string *error)
{
	if (error)
		error->clear();
	if (QFileInfo(QString::fromUtf8(projectFile.c_str())).absoluteFilePath().compare(
		    QFileInfo(QString::fromUtf8(outputFile.c_str())).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
		if (error)
			*error = "Legacy export output must not overwrite the HKProject v2 source";
		return false;
	}
	json_t *project = LoadProjectWithRecovery(projectFile, error);
	if (!project)
		return false;
	const char *format = json_string_value(json_object_get(project, "format"));
	json_t *schemaVersion = json_object_get(project, "schema_version");
	if (!json_is_integer(schemaVersion) || json_integer_value(schemaVersion) != 2 ||
	    !format || strcmp(format, "HKProject") != 0) {
		if (error)
			*error = "Unsupported HKProject schema or format";
		json_decref(project);
		return false;
	}
	json_t *legacy = BuildLegacySceneCollection(project, error);
	const bool saved = legacy && SaveJsonAtomic(legacy, outputFile, error);
	if (legacy)
		json_decref(legacy);
	json_decref(project);
	return saved;
}

bool ValidateLegacyMigration(const std::string &workspaceFile, const std::string &sceneCollectionFile,
			     const std::string &projectFile, MigrationStats *stats, std::string *error)
{
	if (!MigrateLegacyProject(workspaceFile, sceneCollectionFile, projectFile, stats, error))
		return false;
	const std::string exportedFile = projectFile + ".roundtrip.json";
	if (!ExportLegacySceneCollection(projectFile, exportedFile, error))
		return false;

	json_t *original = LoadJson(sceneCollectionFile, error);
	json_t *roundTrip = LoadJson(exportedFile, error);
	const bool equivalent = original && roundTrip && json_equal(original, roundTrip);
	if (original)
		json_decref(original);
	if (roundTrip)
		json_decref(roundTrip);
	QFile::remove(QString::fromUtf8(exportedFile.c_str()));
	if (!equivalent && error && error->empty())
		*error = "Migrated HKProject v2 does not reconstruct the legacy scene collection exactly";
	return equivalent;
}

bool LoadProjectWorkspace(const std::string &projectFile, const std::string &legacyWorkspaceFile,
			  const std::string &legacySceneCollectionFile, std::string *workspaceJson,
			  std::string *error)
{
	if (error)
		error->clear();
	if (!workspaceJson) {
		if (error)
			*error = "Workspace output buffer is null";
		return false;
	}

	if (!QFileInfo::exists(QString::fromUtf8(projectFile.c_str())) &&
	    !MigrateLegacyProject(legacyWorkspaceFile, legacySceneCollectionFile, projectFile, nullptr, error))
		return false;

	json_t *project = LoadProjectWithRecovery(projectFile, error);
	if (!project)
		return false;
	const char *format = json_string_value(json_object_get(project, "format"));
	json_t *version = json_object_get(project, "schema_version");
	json_t *workspace = json_object_get(project, "workspaces");
	if (!format || strcmp(format, "HKProject") != 0 || !json_is_integer(version) ||
	    json_integer_value(version) != 2 || !json_is_object(workspace)) {
		if (error)
			*error = "HKProject v2 is missing its workspace section or has an unsupported schema";
		json_decref(project);
		return false;
	}
	char *serialized = json_dumps(workspace, JSON_INDENT(2) | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
	if (!serialized) {
		if (error)
			*error = "Could not serialize HKProject workspace data";
		json_decref(project);
		return false;
	}
	*workspaceJson = serialized;
	free(serialized);
	json_decref(project);

	if (!legacyWorkspaceFile.empty()) {
		json_error_t parseError{};
		json_t *projection = json_loads(workspaceJson->c_str(), JSON_REJECT_DUPLICATES, &parseError);
		if (projection) {
			std::string ignoredError;
			SaveJsonAtomic(projection, legacyWorkspaceFile, &ignoredError);
			json_decref(projection);
		}
	}
	return true;
}

bool SaveProjectWorkspace(const std::string &projectFile, const std::string &legacyWorkspaceFile,
			  const std::string &legacySceneCollectionFile, const std::string &workspaceJson,
			  std::string *error)
{
	if (error)
		error->clear();
	if (projectFile.empty() || workspaceJson.empty() ||
	    QFileInfo(QString::fromUtf8(projectFile.c_str())).absoluteFilePath().compare(
		    QFileInfo(QString::fromUtf8(legacyWorkspaceFile.c_str())).absoluteFilePath(), Qt::CaseInsensitive) == 0 ||
	    QFileInfo(QString::fromUtf8(projectFile.c_str())).absoluteFilePath().compare(
		    QFileInfo(QString::fromUtf8(legacySceneCollectionFile.c_str())).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
		if (error)
			*error = "HKProject path must be separate from its legacy workspace and scene files";
		return false;
	}

	if (!QFileInfo::exists(QString::fromUtf8(projectFile.c_str())) &&
	    !MigrateLegacyProject(legacyWorkspaceFile, legacySceneCollectionFile, projectFile, nullptr, error))
		return false;
	json_t *project = LoadProjectWithRecovery(projectFile, error);
	json_error_t workspaceError{};
	json_t *workspace = json_loads(workspaceJson.c_str(), JSON_REJECT_DUPLICATES, &workspaceError);
	const char *format = project ? json_string_value(json_object_get(project, "format")) : nullptr;
	json_t *version = project ? json_object_get(project, "schema_version") : nullptr;
	if (!project || !workspace || !json_is_array(json_object_get(workspace, "workspaces")) || !format ||
	    strcmp(format, "HKProject") != 0 || !json_is_integer(version) || json_integer_value(version) != 2) {
		if (error && error->empty())
			*error = "Could not parse current workspace data for HKProject save";
		if (project)
			json_decref(project);
		if (workspace)
			json_decref(workspace);
		return false;
	}
	const char *active = json_string_value(json_object_get(workspace, "active_workspace"));
	json_t *mergedWorkspace = MergePreservingUnknown(json_object_get(project, "workspaces"), workspace);
	if (!mergedWorkspace) {
		if (error)
			*error = "Could not merge current workspace data with the canonical project";
		json_decref(project);
		json_decref(workspace);
		return false;
	}
	json_object_set_new(project, "workspaces", mergedWorkspace);
	json_object_set_new(project, "active_workspace_id", json_string(active ? active : ""));
	const bool saved = SaveJsonAtomic(project, projectFile, error);
	if (saved && !legacyWorkspaceFile.empty()) {
		std::string ignoredError;
		SaveJsonAtomic(mergedWorkspace, legacyWorkspaceFile, &ignoredError);
	}
	json_decref(project);
	json_decref(workspace);
	return saved;
}

bool SaveProjectSceneCollection(const std::string &projectFile, const std::string &legacyWorkspaceFile,
				const std::string &legacySceneCollectionFile, const std::string &workspaceJson,
				const std::string &sceneCollectionJson, std::string *error)
{
	if (error)
		error->clear();
	if (projectFile.empty() || sceneCollectionJson.empty() ||
	    QFileInfo(QString::fromUtf8(projectFile.c_str())).absoluteFilePath().compare(
		    QFileInfo(QString::fromUtf8(legacySceneCollectionFile.c_str())).absoluteFilePath(), Qt::CaseInsensitive) == 0 ||
	    QFileInfo(QString::fromUtf8(projectFile.c_str())).absoluteFilePath().compare(
		    QFileInfo(QString::fromUtf8(legacyWorkspaceFile.c_str())).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
		if (error)
			*error = "HKProject path must be separate from its legacy scene file";
		return false;
	}

	json_t *project = nullptr;
	bool initializedFromCurrentScene = false;
	if (QFileInfo::exists(QString::fromUtf8(projectFile.c_str()))) {
		project = LoadProjectWithRecovery(projectFile, error);
	} else {
		json_error_t workspaceError{}, sceneError{};
		json_t *workspace = json_loads(workspaceJson.c_str(), JSON_REJECT_DUPLICATES, &workspaceError);
		json_t *sceneCollection = json_loads(sceneCollectionJson.c_str(), JSON_REJECT_DUPLICATES, &sceneError);
		if (workspace && sceneCollection)
			project = BuildProjectDocument(workspace, sceneCollection, nullptr, error);
		initializedFromCurrentScene = project != nullptr;
		if (workspace)
			json_decref(workspace);
		if (sceneCollection)
			json_decref(sceneCollection);
		if (!project && error && error->empty())
			*error = "Could not initialize HKProject v2 from the current documents";
	}
	if (!project)
		return false;

	json_error_t sceneError{};
	json_t *sceneCollection = json_loads(sceneCollectionJson.c_str(), JSON_REJECT_DUPLICATES, &sceneError);
	json_error_t workspaceError{};
	json_t *workspace = json_loads(workspaceJson.c_str(), JSON_REJECT_DUPLICATES, &workspaceError);
	const char *format = json_string_value(json_object_get(project, "format"));
	json_t *version = json_object_get(project, "schema_version");
	const bool validProject = format && strcmp(format, "HKProject") == 0 && json_is_integer(version) &&
				  json_integer_value(version) == 2;
	const bool validWorkspace = workspace && json_is_array(json_object_get(workspace, "workspaces"));
	if (!sceneCollection || !validProject || !validWorkspace ||
	    (!initializedFromCurrentScene && !ReplaceProjectSceneCollection(project, sceneCollection, error))) {
		if (error && error->empty())
			*error = "Could not parse the current workspace, scene collection, or project schema";
		if (sceneCollection)
			json_decref(sceneCollection);
		if (workspace)
			json_decref(workspace);
		json_decref(project);
		return false;
	}
	const char *activeWorkspace = json_string_value(json_object_get(workspace, "active_workspace"));
	json_t *mergedWorkspace = MergePreservingUnknown(json_object_get(project, "workspaces"), workspace);
	if (!mergedWorkspace) {
		if (error)
			*error = "Could not merge current workspace data with the canonical project";
		json_decref(sceneCollection);
		json_decref(workspace);
		json_decref(project);
		return false;
	}
	json_object_set_new(project, "workspaces", mergedWorkspace);
	json_object_set_new(project, "active_workspace_id", json_string(activeWorkspace ? activeWorkspace : ""));
	json_decref(sceneCollection);
	json_decref(workspace);
	if (!SaveJsonAtomic(project, projectFile, error)) {
		json_decref(project);
		return false;
	}
	if (!legacyWorkspaceFile.empty()) {
		std::string projectionError;
		if (!SaveJsonAtomic(json_object_get(project, "workspaces"), legacyWorkspaceFile, &projectionError) && error)
			*error = "HKProject was saved, but its workspace compatibility file could not be refreshed: " +
				 projectionError;
	}

	json_t *legacy = BuildLegacySceneCollection(project, error);
	if (legacy) {
		std::string projectionError;
		if (!SaveJsonAtomic(legacy, legacySceneCollectionFile, &projectionError) && error)
			*error = "HKProject was saved, but its OBS compatibility file could not be refreshed: " + projectionError;
		json_decref(legacy);
	}
	json_decref(project);
	return true;
}

} // namespace HKProject

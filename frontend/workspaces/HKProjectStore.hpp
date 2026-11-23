#pragma once

#include <cstddef>
#include <string>

namespace HKProject {

struct MigrationStats {
	size_t workspaces = 0;
	size_t resources = 0;
	size_t scenes = 0;
	size_t groups = 0;
	size_t sceneItems = 0;
	size_t unresolvedSourceReferences = 0;
};

// Creates a versioned, normalized project document from the two legacy files.
// The inputs are never modified. Existing OBS source definitions are stored as
// resources and scenes; scene items keep their source UUID references and
// scene-owned transform/filter data.
bool MigrateLegacyProject(const std::string &workspaceFile, const std::string &sceneCollectionFile,
			  const std::string &outputFile, MigrationStats *stats, std::string *error);

// Reconstructs the OBS scene-collection JSON from a v2 project document.
bool ExportLegacySceneCollection(const std::string &projectFile, const std::string &outputFile,
				 std::string *error);

// Verifies the v2 document and that its legacy scene-collection export is
// semantically identical to the source file used during migration.
bool ValidateLegacyMigration(const std::string &workspaceFile, const std::string &sceneCollectionFile,
			     const std::string &projectFile, MigrationStats *stats, std::string *error);

// Loads workspace metadata from the canonical project, migrating the two legacy
// files once when no project exists. The returned document keeps the legacy
// workspace shape expected by OBSWorkspaceManager.
bool LoadProjectWorkspace(const std::string &projectFile, const std::string &legacyWorkspaceFile,
			  const std::string &legacySceneCollectionFile, std::string *workspaceJson,
			  std::string *error);

// Updates one project section atomically, then refreshes its compatibility
// projection. HKProject v2 remains the authoritative copy.
bool SaveProjectWorkspace(const std::string &projectFile, const std::string &legacyWorkspaceFile,
			  const std::string &legacySceneCollectionFile, const std::string &workspaceJson,
			  std::string *error);
bool SaveProjectSceneCollection(const std::string &projectFile, const std::string &legacyWorkspaceFile,
				const std::string &legacySceneCollectionFile, const std::string &workspaceJson,
				const std::string &sceneCollectionJson, std::string *error);

} // namespace HKProject

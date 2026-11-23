#pragma once
#include <string>
#include <vector>
#include <map>
#include <obs.h>

struct OBSWorkspaceVideoConfig {
    uint32_t base_cx   = 1920;
    uint32_t base_cy   = 1080;
    uint32_t output_cx = 1920;
    uint32_t output_cy = 1080;
    uint32_t fps_num   = 30;
    uint32_t fps_den   = 1;
};

struct SceneItemTransform {
    std::string name;
    int64_t     id = 0;
    float       pos_x = 0.0f;
    float       pos_y = 0.0f;
    float       scale_x = 1.0f;
    float       scale_y = 1.0f;
    float       rot = 0.0f;
    uint32_t    alignment = 5;
    uint32_t    bounds_type = 0;
    uint32_t    bounds_alignment = 0;
    float       bounds_x = 0.0f;
    float       bounds_y = 0.0f;
    bool        crop_to_bounds = false;
    int         crop_left = 0;
    int         crop_top = 0;
    int         crop_right = 0;
    int         crop_bottom = 0;
    bool        visible = true;
};

struct OBSInteractiveSceneConfig {
    std::string source_name;
    std::string fallback_scene;
    bool enabled = true;
};

struct OBSWorkspace {
    std::string id;
    std::string name;
    std::string icon;
    OBSWorkspaceVideoConfig video;
    std::string              active_scene;
    bool interactive_mode_enabled = true;
    std::vector<std::string> scenes;
    std::vector<std::string> hidden_scenes;
    std::map<std::string, std::vector<std::string>> hidden_scenes_by_aspect;
    std::vector<std::string> core_sources;
    std::vector<std::string> variable_sources;
    std::map<std::string, std::pair<uint32_t, uint32_t>> scene_resolutions;
    // scene_name -> (aspect_ratio_key -> list of item transforms)
    std::map<std::string, std::map<std::string, std::vector<SceneItemTransform>>> scene_layouts;
    std::string base_scene;
    std::map<std::string, OBSInteractiveSceneConfig> interactive_scenes; // scene -> trigger source + fixed fallback
    // scene_name -> (element_id -> hex_color)
    std::map<std::string, std::map<std::string, std::string>> scene_text_colors;
};

class OBSWorkspaceManager {
public:
    OBSWorkspaceManager();
    ~OBSWorkspaceManager() = default;

    bool Load(const std::string &path = "", const std::string &sceneCollectionPath = "",
              const std::string &projectPath = "");
    bool Save(const std::string &path = "");
    bool SaveSceneCollectionJson(const std::string &sceneCollectionJson, const std::string &legacyPath);
    std::string GetProjectPath() const { return project_path_; }
    std::string GetSerializedJson() const { return SerializeJson(); }
    bool SwitchTo(const std::string &id);

    static std::string GetWorkspacesDirPath();
    static std::string GetWorkspaceFilePath(const std::string &fileName);
    static std::string GetProjectFilePath(const std::string &sceneCollectionFileName);
    static std::string GetStylesDirPath();
    static std::string GetStyleFilePath(const std::string &relativePath);
    static std::string GetAspectRatioKey(uint32_t cx, uint32_t cy);
    static bool IsAspectKey(const std::string &str);
    static std::string GetLogicalSceneName(const std::string &name);
    static std::string GetLogicalSourceName(const std::string &name);
    static std::string GetBackingSceneName(const std::string &logicalName, const std::string &aspect_key,
                                           const std::string &workspace_id = "", uint32_t cx = 0, uint32_t cy = 0);
    static std::string GetOrCreateBackingScene(const std::string &logicalName, const std::string &aspect_key,
                                                const std::string &workspace_id = "", uint32_t cx = 0, uint32_t cy = 0,
                                                const std::string &templateWorkspaceId = "");
    static bool MigrateSharedCameraCropToSceneItems();
    static std::string GetActiveAspectRatioKey();

    bool SetSourceCategory(const std::string &ws_id, const std::string &source_name, int category);
    int  GetSourceCategory(const std::string &ws_id, const std::string &source_name) const;
    bool SetWorkspaceSources(const std::string &ws_id, const std::vector<std::string> &core, const std::vector<std::string> &variable);

    const std::vector<OBSWorkspace> &GetWorkspaces() const { return workspaces_; }
    const OBSWorkspace *GetActive() const;
    OBSWorkspace       *GetById(const std::string &id);
    void SetActiveScene(const std::string &ws_id, const std::string &scene_name);
    void SetWorkspaceResolution(const std::string &ws_id, uint32_t cx, uint32_t cy);
    void SetSceneResolution(const std::string &ws_id, const std::string &scene_name, uint32_t cx, uint32_t cy);
    bool GetSceneResolution(const std::string &ws_id, const std::string &scene_name, uint32_t &cx, uint32_t &cy) const;

    void SaveSceneLayout(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key, const std::vector<SceneItemTransform> &items);
    bool GetSceneLayout(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key, std::vector<SceneItemTransform> &out_items) const;

    void SetBaseScene(const std::string &ws_id, const std::string &scene_name);
    std::string GetBaseScene(const std::string &ws_id) const;
    void SetInteractiveScene(const std::string &ws_id, const std::string &scene_name,
                             const std::string &source_name, const std::string &fallback_scene);
    bool SetInteractiveSceneEnabled(const std::string &ws_id, const std::string &scene_name, bool enabled);
    bool IsInteractiveSceneEnabled(const std::string &ws_id, const std::string &scene_name) const;
    std::string GetInteractiveSource(const std::string &ws_id, const std::string &scene_name) const;
    std::string GetInteractiveFallbackScene(const std::string &ws_id, const std::string &scene_name) const;
    bool ClearSceneRole(const std::string &ws_id, const std::string &scene_name);
    bool IsBaseScene(const std::string &ws_id, const std::string &scene_name) const;
    bool IsInteractiveScene(const std::string &ws_id, const std::string &scene_name) const;
    std::string FindConflictSceneForSource(const std::string &ws_id, const std::string &source_name, const std::string &current_scene) const;

    bool DuplicateWorkspace(const std::string &sourceId, const std::string &newName, std::string &outNewId);
    bool ToggleSceneHidden(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key = "");
    bool IsSceneHidden(const std::string &ws_id, const std::string &scene_name, const std::string &aspect_key = "") const;

    void SetSceneTextColor(const std::string &ws_id, const std::string &scene_name, const std::string &element_id, const std::string &color_hex);
    std::string GetSceneTextColor(const std::string &ws_id, const std::string &scene_name, const std::string &element_id) const;
    std::map<std::string, std::string> GetSceneTextColors(const std::string &ws_id, const std::string &scene_name) const;
    void SetSceneTextColors(const std::string &ws_id, const std::string &scene_name, const std::map<std::string, std::string> &colors);
    void ApplySceneTextColors(const std::string &ws_id, const std::string &scene_name);

private:
    std::vector<OBSWorkspace> workspaces_;
    std::string               active_id_;
    std::string               json_path_;
    std::string               scene_collection_path_;
    std::string               project_path_;
    bool                      canonical_project_loaded_ = false;
    void LoadDefaults();
    bool ParseJson(const char *json_str);
    std::string SerializeJson() const;
};

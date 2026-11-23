#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <QObject>
#include <QString>

struct DeletedItem {
	std::string id;          // Unique ID
	std::string type;        // "scene" or "source"
	std::string name;        // Name of the deleted scene or source
	std::string parentScene; // For sources, the scene it belonged to
	std::string workspaceId; // Workspace ID
	int64_t deletedAt = 0;   // Epoch seconds
	std::string jsonData;    // Full serialized source / scene JSON
	std::string extraJson;   // Transform / crop / layout JSON
};

class RecycleBin : public QObject {
	Q_OBJECT

public:
	static RecycleBin &Instance();

	void AddDeletedScene(const std::string &sceneName,
			     const std::string &wsId,
			     const std::string &jsonData,
			     const std::string &extraJson = "");

	void AddDeletedSource(const std::string &sourceName,
			      const std::string &parentScene,
			      const std::string &jsonData,
			      const std::string &extraJson = "");

	std::vector<DeletedItem> GetItems();
	bool RestoreItem(const std::string &id, QString &errorMsg);
	bool PermanentlyDeleteItem(const std::string &id);
	void ClearAll();

	int GetCount();

signals:
	void ItemsChanged();

private:
	RecycleBin();
	~RecycleBin() = default;

	std::string GetFilePath() const;
	void Load();
	void Save();
	void PurgeExpired(); // Purges items older than 24 hours (86400 seconds)

	std::vector<DeletedItem> items_;
};

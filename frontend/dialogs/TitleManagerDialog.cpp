#include "TitleManagerDialog.hpp"
#include <widgets/OBSBasic.hpp>
#include "workspaces/WorkspaceModel.hpp"
#include <obs.h>
#include <obs-frontend-api.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QMessageBox>
#include <QFontDialog>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QTimer>
#include <QStyle>
#include <util/platform.h>
#include <util/base.h>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
struct TemporaryTitleSourceState {
	obs_source_t *source = nullptr;
	bool hadText = false;
	std::string text;
	bool hadExtentsWrap = false;
	bool extentsWrap = false;
	bool hadChatlog = false;
	bool chatlog = false;
	bool hadFont = false;
	obs_data_t *font = nullptr;
	bool hadTightBounds = false;
	bool tightBounds = false;
};

std::vector<TemporaryTitleSourceState> temporaryTitleSourceStates;
bool temporaryTitleActive = false;
QString currentTemporaryTitle;
QString currentTemporaryFont;
int currentTemporaryFontSize = 0;
bool currentTemporaryBold = false;

struct TitleImageVisibilityContext {
	QString keepVisibleName;
};

bool HideOtherTitleImages(obs_scene_t *, obs_sceneitem_t *item, void *opaque)
{
	auto *context = static_cast<TitleImageVisibilityContext *>(opaque);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source) return true;
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	if (name.startsWith(QString::fromUtf8("HK عنوان صورة - ")) && name != context->keepVisibleName)
		obs_sceneitem_set_visible(item, false);
	return true;
}

void ApplyTitleImageToCurrentScene(const QString &title, const QString &imagePath)
{
	OBSBasic *main = OBSBasic::Get();
	OBSScene scene = main ? main->GetCurrentScene() : nullptr;
	if (!scene || imagePath.isEmpty() || !QFile::exists(imagePath)) {
		if (!imagePath.isEmpty() && !QFile::exists(imagePath))
			blog(LOG_WARNING, "[TitleManager] Title image does not exist: %s", imagePath.toUtf8().constData());
		return;
	}

	const QString sourceName = QString::fromUtf8("HK عنوان صورة - ") + title.trimmed();
	TitleImageVisibilityContext visibilityContext{sourceName};
	obs_scene_enum_items(scene, HideOtherTitleImages, &visibilityContext);

	const char *sourceId = obs_get_latest_input_type_id("image_source");
	if (!sourceId) return;
	obs_source_t *source = obs_get_source_by_name(sourceName.toUtf8().constData());
	bool created = false;
	if (!source) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "file", imagePath.toUtf8().constData());
		source = obs_source_create(sourceId, sourceName.toUtf8().constData(), settings, nullptr);
		obs_data_release(settings);
		created = source != nullptr;
	}
	if (!source) return;

	if (!created) {
		obs_data_t *settings = obs_source_get_settings(source);
		obs_data_set_string(settings, "file", imagePath.toUtf8().constData());
		obs_source_update(source, settings);
		obs_data_release(settings);
	}

	obs_sceneitem_t *item = obs_scene_find_source(scene, sourceName.toUtf8().constData());
	if (!item) {
		item = obs_scene_add(scene, source);
		if (item) {
			obs_video_info videoInfo;
			if (obs_get_video_info(&videoInfo)) {
				vec2 center;
				center.x = static_cast<float>(videoInfo.base_width) * 0.5f;
				center.y = static_cast<float>(videoInfo.base_height) * 0.5f;
				obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);
				obs_sceneitem_set_pos(item, &center);
			}
		}
	}
	if (item) {
		obs_sceneitem_set_visible(item, true);
		obs_sceneitem_select(item, true);
	} else if (created) {
		obs_source_remove(source);
	}
	obs_source_release(source);
}

bool IsManagedTitleSource(obs_source_t *source)
{
	if (!source) return false;
	const char *id = obs_source_get_id(source);
	if (!id || (strncmp(id, "text_gdiplus", 12) != 0 && strncmp(id, "text_ft2_source", 15) != 0))
		return false;

	const QString name = QString::fromUtf8(obs_source_get_name(source)).trimmed();
	const QString lower = name.toLower();

	// Exclude non-title text sources like timer, clock, counter, date
	if (name.contains(QString::fromUtf8("عداد")) ||
	    name.contains(QString::fromUtf8("ساعة")) ||
	    name.contains(QString::fromUtf8("تاريخ")) ||
	    name.contains(QString::fromUtf8("تنبيه")) ||
	    lower.contains("timer") ||
	    lower.contains("clock") ||
	    lower.contains("counter") ||
	    lower.contains("time") ||
	    lower.contains("date") ||
	    lower.contains("alert")) {
		return false;
	}

	// 1. Explicit title roots commonly used in scenes
	for (const QString &root : {QString::fromUtf8("العنوان"),
	                           QString::fromUtf8("عنوان الدرس"),
	                           QString::fromUtf8("نص (GDI+)"),
	                           QString::fromUtf8("عنوان"),
	                           QString::fromUtf8("الترمذي"),
	                           QString::fromUtf8("نونية ابن القيم"),
	                           QString::fromUtf8("نونية"),
	                           QString::fromUtf8("مصابيح"),
	                           QString::fromUtf8("الوجيز")}) {
		if (name == root || name.startsWith(root + " [") || name.startsWith(root))
			return true;
	}

	// 2. Name contains keywords
	if (name.contains(QString::fromUtf8("عنوان")) ||
	    name.contains(QString::fromUtf8("درس")) ||
	    lower.contains("title")) {
		return true;
	}

	// 3. Current source text matches any saved title
	obs_data_t *settings = obs_source_get_settings(source);
	QString curText = QString::fromUtf8(obs_data_get_string(settings, "text")).trimmed();
	obs_data_release(settings);
	if (!curText.isEmpty()) {
		bool isTimer = false;
		if (curText.length() <= 8 && curText.contains(':')) {
			bool allDigitsOrColon = true;
			for (const QChar &c : curText) {
				if (!c.isDigit() && c != ':') {
					allDigitsOrColon = false;
					break;
				}
			}
			isTimer = allDigitsOrColon;
		}
		if (isTimer) return false;

		const auto saved = TitleManagerDialog::LoadTitlesData();
		for (const auto &e : saved) {
			if (curText.contains(e.text.trimmed()) || e.text.trimmed().contains(curText))
				return true;
		}
	}

	// Captions and other text retain their content unless identified as titles.
	return false;
}

std::vector<obs_source_t *> ManagedTitleSources()
{
	std::vector<obs_source_t *> sources;
	obs_enum_sources([](void *param, obs_source_t *source) {
		if (IsManagedTitleSource(source))
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(source));
		return true;
	}, &sources);
	return sources;
}

void RestoreCapturedTemporaryTitleSources()
{
	for (auto &state : temporaryTitleSourceStates) {
		if (!state.source) continue;

		obs_data_t *settings = obs_source_get_settings(state.source);
		if (state.hadText) {
			obs_data_set_string(settings, "text", state.text.c_str());
		} else {
			obs_data_unset_user_value(settings, "text");
		}
		if (state.hadExtentsWrap) {
			obs_data_set_bool(settings, "extents_wrap", state.extentsWrap);
		} else {
			obs_data_unset_user_value(settings, "extents_wrap");
		}
		if (state.hadChatlog) {
			obs_data_set_bool(settings, "chatlog", state.chatlog);
		} else {
			obs_data_unset_user_value(settings, "chatlog");
		}
		if (state.hadFont && state.font) {
			obs_data_set_obj(settings, "font", state.font);
		} else {
			obs_data_unset_user_value(settings, "font");
		}
		if (state.hadTightBounds) {
			obs_data_set_bool(settings, "tight_bounds", state.tightBounds);
		} else {
			obs_data_unset_user_value(settings, "tight_bounds");
		}
		obs_source_update(state.source, settings);
		obs_data_release(settings);
		if (state.font) obs_data_release(state.font);
		obs_source_release(state.source);
	}
	temporaryTitleSourceStates.clear();
}

void CaptureTemporaryTitleSource(obs_source_t *source)
{
	if (!source) return;

	for (const auto &state : temporaryTitleSourceStates) {
		if (state.source == source) return;
	}

	TemporaryTitleSourceState state;
	state.source = obs_source_get_ref(source);
	obs_data_t *settings = obs_source_get_settings(source);
	state.hadText = obs_data_has_user_value(settings, "text");
	state.text = obs_data_get_string(settings, "text");
	state.hadExtentsWrap = obs_data_has_user_value(settings, "extents_wrap");
	state.extentsWrap = obs_data_get_bool(settings, "extents_wrap");
	state.hadChatlog = obs_data_has_user_value(settings, "chatlog");
	state.chatlog = obs_data_get_bool(settings, "chatlog");
	state.hadFont = obs_data_has_user_value(settings, "font");
	state.hadTightBounds = obs_data_has_user_value(settings, "tight_bounds");
	state.tightBounds = obs_data_get_bool(settings, "tight_bounds");
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font) {
		state.font = obs_data_create();
		obs_data_apply(state.font, font);
		obs_data_release(font);
	}
	obs_data_release(settings);
	temporaryTitleSourceStates.emplace_back(std::move(state));
}

void CaptureTemporaryTitleSources()
{
	if (!temporaryTitleSourceStates.empty()) return;

	for (auto source : ManagedTitleSources()) {
		CaptureTemporaryTitleSource(source);
		obs_source_release(source);
	}
}
} // namespace

static QString GetTitlesFilePath()
{
	QString path = QString::fromStdString(OBSWorkspaceManager::GetWorkspaceFilePath("titles.json"));
	if (QFile::exists(path)) {
		return path;
	}
	char abs_path[512];
	if (os_get_abs_path("workspaces/titles.json", abs_path, sizeof(abs_path))) {
		if (QFile::exists(abs_path)) {
			return QString::fromUtf8(abs_path);
		}
	}
	return path;
}

void TitleManagerDialog::ParseTitleString(const QString &raw, TitleEntry &entry)
{
	QString s = raw.trimmed();
	entry.text = s;
	if (s.isEmpty()) {
		entry.baseTitle.clear();
		entry.hasNumber = false;
		entry.lessonNumber = 1;
		entry.separator = " / ";
		return;
	}

	// Look for trailing ASCII or digits
	int len = s.length();
	int i = len - 1;
	while (i >= 0 && s[i].isDigit()) {
		i--;
	}

	if (i < len - 1 && i >= 0) {
		QString numStr = s.mid(i + 1);
		bool ok = false;
		int num = numStr.toInt(&ok);
		if (ok && num > 0) {
			int sepEnd = i;
			int j = sepEnd;
			while (j >= 0 && (s[j] == ' ' || s[j] == '/' || s[j] == '-' || s[j] == ':' || s[j] == '#' || s[j] == '|')) {
				j--;
			}
			if (j >= 0) {
				QString sep = s.mid(j + 1, sepEnd - j);
				QString base = s.left(j + 1).trimmed();
				if (!base.isEmpty()) {
					entry.baseTitle = base;
					entry.hasNumber = true;
					entry.lessonNumber = num;
					if (sep.contains('/')) {
						entry.separator = " / ";
					} else if (sep.contains('-')) {
						entry.separator = " - ";
					} else if (sep.contains(':')) {
						entry.separator = " : ";
					} else {
						entry.separator = " ";
					}
					entry.text = entry.fullTitle();
					return;
				}
			}
		}
	}

	entry.baseTitle = s;
	entry.hasNumber = false;
	entry.lessonNumber = 1;
	entry.separator = " / ";
	entry.text = s;
}

QList<TitleEntry> TitleManagerDialog::LoadTitlesData()
{
	QList<TitleEntry> list;
	QString filePath = GetTitlesFilePath();
	QFile f(filePath);
	if (f.open(QIODevice::ReadOnly)) {
		QByteArray data = f.readAll();
		f.close();

		if (data.startsWith("\xEF\xBB\xBF")) {
			data.remove(0, 3);
		}

		QJsonDocument doc = QJsonDocument::fromJson(data);
		if (doc.isObject()) {
			QJsonObject obj = doc.object();
			QJsonArray arr = obj["titles"].toArray();
			for (const auto &val : arr) {
				if (val.isObject()) {
					QJsonObject to = val.toObject();
					TitleEntry e;
					e.isImage = to["kind"].toString() == "image";
					e.imagePath = to["image_path"].toString();
					e.fontFace = to.contains("font") ? to["font"].toString() : "zain mob-long300";
					e.fontSize = to.contains("size") ? to["size"].toInt() : 120;
					e.isBold = to.contains("bold") ? to["bold"].toBool() : false;

					if (e.isImage) {
						e.baseTitle = to["base_title"].toString();
						e.text = to["text"].toString(e.baseTitle);
						e.hasNumber = false;
					} else if (to.contains("base_title")) {
						e.baseTitle = to["base_title"].toString();
						e.hasNumber = to["has_number"].toBool();
						e.lessonNumber = to["lesson_number"].toInt(1);
						e.separator = to.contains("separator") ? to["separator"].toString() : " / ";
						e.text = e.fullTitle();
					} else {
						QString raw = to["text"].toString();
						ParseTitleString(raw, e);
					}

					if (!e.text.trimmed().isEmpty()) {
						list.append(e);
					}
				} else if (val.isString()) {
					TitleEntry e;
					e.fontFace = "zain mob-long300";
					e.fontSize = 120;
					e.isBold = false;
					ParseTitleString(val.toString(), e);
					if (!e.text.trimmed().isEmpty()) {
						list.append(e);
					}
				}
			}
		}
	}

	if (list.isEmpty()) {
		TitleEntry e1;
		ParseTitleString(QString::fromUtf8("بث مباشر لاستقبال الأسئلة"), e1);
		e1.fontFace = "zain mob-long300"; e1.fontSize = 150;
		list.append(e1);

		TitleEntry e2;
		ParseTitleString(QString::fromUtf8("الوجيز في السنة 78"), e2);
		e2.fontFace = "Andalus"; e2.fontSize = 100;
		list.append(e2);

		TitleEntry e3;
		ParseTitleString(QString::fromUtf8("شرح جامع الترمذي / 24"), e3);
		e3.fontFace = "Andalus"; e3.fontSize = 100;
		list.append(e3);

		TitleEntry e4;
		ParseTitleString(QString::fromUtf8("نونية ابن القيم / 55"), e4);
		e4.fontFace = "Andalus"; e4.fontSize = 100;
		list.append(e4);

		TitleEntry e5;
		ParseTitleString(QString::fromUtf8("مصابيح 187"), e5);
		e5.fontFace = "ALLEXP03SAHLAB"; e5.fontSize = 150;
		list.append(e5);

		TitleEntry e6;
		ParseTitleString(QString::fromUtf8("جلسة إفتاء وأسئلة عامة"), e6);
		e6.fontFace = "zain mob-long300"; e6.fontSize = 140;
		list.append(e6);
	}

	return list;
}

QStringList TitleManagerDialog::LoadTitlesList()
{
	QList<TitleEntry> entries = LoadTitlesData();
	QStringList res;
	for (const auto &e : entries) {
		res.append(e.fullTitle());
	}
	return res;
}

QString TitleManagerDialog::GetActiveTitle()
{
	QString filePath = GetTitlesFilePath();
	QFile f(filePath);
	if (f.open(QIODevice::ReadOnly)) {
		QByteArray data = f.readAll();
		f.close();
		if (data.startsWith("\xEF\xBB\xBF")) data.remove(0, 3);
		QJsonDocument doc = QJsonDocument::fromJson(data);
		if (doc.isObject()) {
			QString active = doc.object()["active_title"].toString();
			if (!active.isEmpty()) return active;
		}
	}

	obs_source_t *src = obs_get_source_by_name("العنوان");
	if (!src) src = obs_get_source_by_name("عنوان الدرس");
	if (src) {
		obs_data_t *settings = obs_source_get_settings(src);
		QString t = QString::fromUtf8(obs_data_get_string(settings, "text"));
		obs_data_release(settings);
		obs_source_release(src);
		if (!t.isEmpty()) return t;
	}
	return QString::fromUtf8("بث مباشر لاستقبال الأسئلة");
}

TitleEntry TitleManagerDialog::GetActiveTitleEntry()
{
	QString active = GetActiveTitle().trimmed();
	QList<TitleEntry> entries = LoadTitlesData();
	for (const auto &e : entries) {
		if (e.text.trimmed() == active || e.fullTitle().trimmed() == active) {
			return e;
		}
	}
	TitleEntry fallback;
	ParseTitleString(active, fallback);
	return fallback;
}

bool TitleManagerDialog::UpdateLessonNumberForActive(int newLessonNumber)
{
	if (newLessonNumber < 1) newLessonNumber = 1;
	QString active = GetActiveTitle().trimmed();
	QList<TitleEntry> entries = LoadTitlesData();
	bool found = false;
	QString newActive;
	QString fontFace;
	int fontSize = 120;
	bool isBold = false;

	for (auto &e : entries) {
		if (e.text.trimmed() == active || e.fullTitle().trimmed() == active) {
			e.hasNumber = true;
			e.lessonNumber = newLessonNumber;
			e.text = e.fullTitle();
			newActive = e.text;
			fontFace = e.fontFace;
			fontSize = e.fontSize;
			isBold = e.isBold;
			found = true;
			break;
		}
	}

	if (!found) {
		TitleEntry e;
		ParseTitleString(active, e);
		e.hasNumber = true;
		e.lessonNumber = newLessonNumber;
		e.text = e.fullTitle();
		newActive = e.text;
		fontFace = e.fontFace;
		fontSize = e.fontSize;
		isBold = e.isBold;
		entries.append(e);
	}

	SaveTitlesData(entries, newActive);
	ApplyTitleToOBS(newActive, fontFace, fontSize, isBold);
	return true;
}

bool TitleManagerDialog::IncrementActiveLessonNumber(int delta)
{
	TitleEntry active = GetActiveTitleEntry();
	int newNum = active.lessonNumber + delta;
	if (newNum < 1) newNum = 1;
	return UpdateLessonNumberForActive(newNum);
}

QString TitleManagerDialog::GetTemporaryTitle()
{
	return temporaryTitleActive ? currentTemporaryTitle : QString();
}

void TitleManagerDialog::GetTemporaryTitleFont(QString &fontFace, int &fontSize, bool &isBold)
{
	fontFace = "zain mob-long300";
	fontSize = 120;
	isBold = false;

	QString path = GetTitlesFilePath();
	QFile file(path);
	if (file.open(QIODevice::ReadOnly)) {
		QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
		if (doc.isObject()) {
			QJsonObject obj = doc.object();
			if (obj.contains("temp_font") && !obj["temp_font"].toString().isEmpty()) {
				fontFace = obj["temp_font"].toString();
			}
			if (obj.contains("temp_size") && obj["temp_size"].toInt() > 0) {
				fontSize = obj["temp_size"].toInt();
			}
			if (obj.contains("temp_bold")) {
				isBold = obj["temp_bold"].toBool();
			}
		}
	}
}

void TitleManagerDialog::SaveTemporaryTitleFont(const QString &fontFace, int fontSize, bool isBold)
{
	if (fontFace.isEmpty() || fontSize <= 0) return;

	QString path = GetTitlesFilePath();
	QJsonObject obj;
	QFile file(path);
	if (file.open(QIODevice::ReadOnly)) {
		QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
		if (doc.isObject()) {
			obj = doc.object();
		}
		file.close();
	}

	obj["temp_font"] = fontFace;
	obj["temp_size"] = fontSize;
	obj["temp_bold"] = isBold;

	QJsonDocument doc(obj);
	QByteArray data = doc.toJson(QJsonDocument::Indented);

	QStringList targets;
	targets << path;
	QString mainPath = QString::fromStdString(OBSWorkspaceManager::GetWorkspaceFilePath("titles.json"));
	if (!targets.contains(mainPath)) targets << mainPath;

	QString appDir = QCoreApplication::applicationDirPath();
	QString runtimePath = appDir + "/../../workspaces/titles.json";
	if (QFileInfo(runtimePath).canonicalFilePath() != QFileInfo(mainPath).canonicalFilePath()) {
		targets << runtimePath;
	}

	char abs_path[512];
	if (os_get_abs_path("workspaces/titles.json", abs_path, sizeof(abs_path))) {
		QString p = QString::fromUtf8(abs_path);
		if (!targets.contains(p)) targets << p;
	}

	for (const auto &p : targets) {
		QFileInfo fi(p);
		QDir().mkpath(fi.dir().absolutePath());
		QFile f(p);
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			f.write(data);
			f.close();
		}
	}
}

void TitleManagerDialog::ApplyTemporaryTitleToOBS(const QString &titleText,
							  const QString &fontFace,
							  int fontSize,
							  bool isBold)
{
	QString usedFont = fontFace;
	int usedSize = fontSize;
	bool usedBold = isBold;
	if (usedFont.isEmpty() || usedSize <= 0) {
		GetTemporaryTitleFont(usedFont, usedSize, usedBold);
	}

	currentTemporaryTitle = titleText;
	currentTemporaryFont = usedFont;
	currentTemporaryFontSize = usedSize;
	currentTemporaryBold = usedBold;
	temporaryTitleActive = true;
	CaptureTemporaryTitleSources();
	ApplyTitleToOBS(titleText, usedFont, usedSize, usedBold);
}

void TitleManagerDialog::RestoreTemporaryTitleFromOBS()
{
	RestoreCapturedTemporaryTitleSources();
	temporaryTitleActive = false;
	currentTemporaryTitle.clear();
	currentTemporaryFont.clear();
	currentTemporaryFontSize = 0;
	currentTemporaryBold = false;
}

bool TitleManagerDialog::SuspendTemporaryTitleForSave()
{
	if (!temporaryTitleActive) return false;
	RestoreCapturedTemporaryTitleSources();
	return true;
}

void TitleManagerDialog::ResumeTemporaryTitleAfterSave()
{
	if (!temporaryTitleActive) return;
	CaptureTemporaryTitleSources();
	ApplyTitleToOBS(currentTemporaryTitle, currentTemporaryFont, currentTemporaryFontSize,
			currentTemporaryBold);
}

bool TitleManagerDialog::SaveTitlesData(const QList<TitleEntry> &entries, const QString &activeTitle)
{
	QJsonObject obj;
	obj["active_title"] = activeTitle;

	QString tempFont; int tempSize = 120; bool tempBold = false;
	GetTemporaryTitleFont(tempFont, tempSize, tempBold);
	obj["temp_font"] = tempFont;
	obj["temp_size"] = tempSize;
	obj["temp_bold"] = tempBold;

	QJsonArray arr;
	for (const auto &e : entries) {
		QJsonObject to;
		to["text"] = e.fullTitle();
		to["kind"] = e.isImage ? "image" : "text";
		if (e.isImage)
			to["image_path"] = e.imagePath;
		to["base_title"] = e.baseTitle;
		to["has_number"] = e.hasNumber;
		to["lesson_number"] = e.lessonNumber;
		to["separator"] = e.separator;
		to["font"] = e.fontFace;
		to["size"] = e.fontSize;
		to["bold"] = e.isBold;
		arr.append(to);
	}
	obj["titles"] = arr;

	QJsonDocument doc(obj);
	QByteArray data = doc.toJson(QJsonDocument::Indented);

	QStringList targets;
	QString mainPath = QString::fromStdString(OBSWorkspaceManager::GetWorkspaceFilePath("titles.json"));
	targets << mainPath;

	QString appDir = QCoreApplication::applicationDirPath();
	QString runtimePath = appDir + "/../../workspaces/titles.json";
	if (QFileInfo(runtimePath).canonicalFilePath() != QFileInfo(mainPath).canonicalFilePath()) {
		targets << runtimePath;
	}

	char abs_path[512];
	if (os_get_abs_path("workspaces/titles.json", abs_path, sizeof(abs_path))) {
		targets << QString::fromUtf8(abs_path);
	}

	bool anySaved = false;
	for (const auto &p : targets) {
		QFileInfo fi(p);
		QDir().mkpath(fi.dir().absolutePath());
		QFile f(p);
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			f.write(data);
			f.close();
			anySaved = true;
		}
	}
	return anySaved;
}

void TitleManagerDialog::ApplyTitleToOBS(const QString &titleText,
                                         const QString &fontFace,
                                         int fontSize,
                                         bool isBold,
                                         bool tightBounds)
{
	for (const auto &entry : LoadTitlesData()) {
		if (entry.isImage && (entry.text.trimmed() == titleText.trimmed() || entry.fullTitle().trimmed() == titleText.trimmed())) {
			ApplyTitleImageToCurrentScene(entry.fullTitle(), entry.imagePath);
			return;
		}
	}
	if (OBSBasic *main = OBSBasic::Get()) {
		if (OBSScene scene = main->GetCurrentScene()) {
			TitleImageVisibilityContext visibilityContext;
			obs_scene_enum_items(scene, HideOtherTitleImages, &visibilityContext);
		}
	}

	QString cleanText = titleText;
	cleanText.replace("\r\n", " ").replace('\r', ' ').replace('\n', ' ').replace('\t', ' ');
	cleanText = cleanText.simplified();
	if (cleanText.isEmpty()) return;

	bool updatedAny = false;

	auto applyToSource = [&](obs_source_t *src, const char *name) {
		obs_data_t *settings = obs_source_get_settings(src);

		// Re-rendering GDI+ text is expensive; startup re-applies the active
		// title to every managed source, so skip sources already showing it.
		const QByteArray textUtf8 = cleanText.toUtf8();
		bool unchanged = strcmp(obs_data_get_string(settings, "text"), textUtf8.constData()) == 0 &&
				 !obs_data_get_bool(settings, "extents_wrap") && !obs_data_get_bool(settings, "chatlog") &&
				 obs_data_get_bool(settings, "tight_bounds") == tightBounds;
		if (unchanged && !fontFace.isEmpty() && fontSize > 0) {
			obs_data_t *current_font = obs_data_get_obj(settings, "font");
			unchanged = current_font &&
				    QString::fromUtf8(obs_data_get_string(current_font, "face")) == fontFace &&
				    obs_data_get_int(current_font, "size") == fontSize &&
				    obs_data_get_int(current_font, "flags") == (isBold ? 1 : 0);
			obs_data_release(current_font);
		}
		if (unchanged) {
			obs_data_release(settings);
			updatedAny = true;
			return;
		}

		obs_data_set_string(settings, "text", textUtf8.constData());
		obs_data_set_bool(settings, "extents_wrap", false);
		obs_data_set_bool(settings, "chatlog", false);
		obs_data_set_bool(settings, "tight_bounds", tightBounds);

		if (!fontFace.isEmpty() && fontSize > 0) {
			obs_data_t *font_obj = obs_data_get_obj(settings, "font");
			if (!font_obj) {
				font_obj = obs_data_create();
			}
			obs_data_set_string(font_obj, "face", fontFace.toUtf8().constData());
			obs_data_set_int(font_obj, "size", fontSize);
			obs_data_set_string(font_obj, "style", isBold ? "Bold" : "Regular");
			int flags = isBold ? 1 : 0;
			obs_data_set_int(font_obj, "flags", flags);
			obs_data_set_obj(settings, "font", font_obj);
			obs_data_release(font_obj);
		}

		obs_source_update(src, settings);
		obs_data_release(settings);

		updatedAny = true;
		blog(LOG_INFO, "[TitleManager] Updated title source '%s': %s (Font: %s, Size: %d, Bold: %d)",
		     name, cleanText.toUtf8().constData(),
		     fontFace.toUtf8().constData(), fontSize, isBold ? 1 : 0);
	};

	for (auto source : ManagedTitleSources()) {
		applyToSource(source, obs_source_get_name(source));
		obs_source_release(source);
	}

	if (updatedAny) {
		if (OBSBasic *main = OBSBasic::Get()) {
			main->FitTextItemsToSmartFrame();
		}
	} else {
		blog(LOG_WARNING, "[TitleManager] Could not find any candidate title source to update");
	}
}

// -------------------------------------------------------
// TitleManagerDialog Implementation
// -------------------------------------------------------
TitleManagerDialog::TitleManagerDialog(OBSBasic *main, QWidget *parent)
	: QDialog(parent), main_(main)
{
	setWindowTitle(QString::fromUtf8("نظام إدارة العناوين وسلاسل الدروس — HK Stream"));
	setLayoutDirection(Qt::RightToLeft);
	setMinimumSize(600, 720);
	resize(640, 780);

	titlesData_ = LoadTitlesData();
	activeTitle_ = GetActiveTitle();

	buildUI();
	applyStylesheet();
	refreshList();
}

TitleEntry *TitleManagerDialog::findEntry(const QString &text)
{
	for (auto &e : titlesData_) {
		if (e.text == text || e.fullTitle() == text) return &e;
	}
	return nullptr;
}

TitleEntry *TitleManagerDialog::currentSelectedEntry()
{
	if (!comboTitles_ || comboTitles_->currentIndex() < 0) return nullptr;
	QString key = comboTitles_->currentData().toString();
	return findEntry(key);
}

void TitleManagerDialog::buildUI()
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(14, 14, 14, 14);
	layout->setSpacing(10);

	// Header banner and instructions removed per user request
	lblCurrentActive_ = new QLabel(this);
	lblCurrentActive_->setVisible(false);

	// Titles Dropdown Selector (قائمة العناوين المنسدلة)
	auto *titleRow = new QHBoxLayout();
	auto *lblChoose = new QLabel(QString::fromUtf8("العنوان الحالي / المحدد:"), this);
	lblChoose->setStyleSheet("font-weight: bold; font-size: 13px;");
	titleRow->addWidget(lblChoose);

	comboTitles_ = new QComboBox(this);
	comboTitles_->setObjectName("comboTitles");
	comboTitles_->setLayoutDirection(Qt::RightToLeft);
	comboTitles_->setFixedHeight(34);
	connect(comboTitles_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TitleManagerDialog::onTitleSelected);
	titleRow->addWidget(comboTitles_, 1);
	layout->addLayout(titleRow);

	// Specialized Title & Lesson Details Panel
	groupTitleDetails_ = new QGroupBox(QString::fromUtf8("📚 تفاصيل العنوان ورقم الدرس المحدد:"), this);
	groupTitleDetails_->setObjectName("groupTitleDetails");
	groupTitleDetails_->setLayoutDirection(Qt::RightToLeft);
	auto *detailsLayout = new QVBoxLayout(groupTitleDetails_);
	detailsLayout->setContentsMargins(10, 12, 10, 10);
	detailsLayout->setSpacing(8);

	// Row 1: Base Title Edit
	auto *rowBase = new QHBoxLayout();
	rowBase->setSpacing(8);
	auto *lblBase = new QLabel(QString::fromUtf8("اسم الدرس / السلسلة:"), groupTitleDetails_);
	rowBase->addWidget(lblBase);

	txtEditBaseTitle_ = new QLineEdit(groupTitleDetails_);
	txtEditBaseTitle_->setObjectName("txtEditBaseTitle");
	txtEditBaseTitle_->setFixedHeight(30);
	txtEditBaseTitle_->setPlaceholderText(QString::fromUtf8("مثال: شرح جامع الترمذي"));
	connect(txtEditBaseTitle_, &QLineEdit::textChanged, this, &TitleManagerDialog::onLessonNumberChanged);
	rowBase->addWidget(txtEditBaseTitle_, 1);
	detailsLayout->addLayout(rowBase);

	// Row 2: Has Number + Lesson SpinBox + Quick Increment / Decrement + Separator
	auto *rowNumber = new QHBoxLayout();
	rowNumber->setSpacing(8);

	chkHasNumber_ = new QCheckBox(QString::fromUtf8("سلسلة مرقمة (تفعيل رقم الدرس)"), groupTitleDetails_);
	chkHasNumber_->setObjectName("chkHasNumber");
	connect(chkHasNumber_, &QCheckBox::toggled, this, &TitleManagerDialog::onLessonNumberChanged);
	rowNumber->addWidget(chkHasNumber_);

	auto *lblNum = new QLabel(QString::fromUtf8("رقم الدرس:"), groupTitleDetails_);
	rowNumber->addWidget(lblNum);

	spinLessonNum_ = new QSpinBox(groupTitleDetails_);
	spinLessonNum_->setObjectName("spinLessonNum");
	spinLessonNum_->setRange(1, 99999);
	spinLessonNum_->setValue(1);
	spinLessonNum_->setFixedHeight(30);
	spinLessonNum_->setFixedWidth(80);
	spinLessonNum_->setAlignment(Qt::AlignCenter);
	connect(spinLessonNum_, QOverload<int>::of(&QSpinBox::valueChanged), this, &TitleManagerDialog::onLessonNumberChanged);
	rowNumber->addWidget(spinLessonNum_);

	btnIncLesson_ = new QPushButton(QString::fromUtf8("+1"), groupTitleDetails_);
	btnIncLesson_->setObjectName("btnIncLesson");
	btnIncLesson_->setFixedSize(36, 30);
	btnIncLesson_->setCursor(Qt::PointingHandCursor);
	btnIncLesson_->setToolTip(QString::fromUtf8("زيادة رقم الدرس (+1)"));
	connect(btnIncLesson_, &QPushButton::clicked, this, &TitleManagerDialog::onIncrementLessonClicked);
	rowNumber->addWidget(btnIncLesson_);

	btnDecLesson_ = new QPushButton(QString::fromUtf8("-1"), groupTitleDetails_);
	btnDecLesson_->setObjectName("btnDecLesson");
	btnDecLesson_->setFixedSize(36, 30);
	btnDecLesson_->setCursor(Qt::PointingHandCursor);
	btnDecLesson_->setToolTip(QString::fromUtf8("إنقاص رقم الدرس (-1)"));
	connect(btnDecLesson_, &QPushButton::clicked, this, &TitleManagerDialog::onDecrementLessonClicked);
	rowNumber->addWidget(btnDecLesson_);

	auto *lblSep = new QLabel(QString::fromUtf8("الفاصل:"), groupTitleDetails_);
	rowNumber->addWidget(lblSep);

	comboSeparator_ = new QComboBox(groupTitleDetails_);
	comboSeparator_->setObjectName("comboSeparator");
	comboSeparator_->setFixedHeight(30);
	comboSeparator_->addItem(QString::fromUtf8(" /  (شرطة مائلة)"), QString(" / "));
	comboSeparator_->addItem(QString::fromUtf8(" -  (شرطة)"), QString(" - "));
	comboSeparator_->addItem(QString::fromUtf8(" :  (نقطتان)"), QString(" : "));
	comboSeparator_->addItem(QString::fromUtf8("مسافة فقط"), QString(" "));
	connect(comboSeparator_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TitleManagerDialog::onLessonNumberChanged);
	rowNumber->addWidget(comboSeparator_);

	detailsLayout->addLayout(rowNumber);

	// Row 3: Live Preview
	auto *rowPreview = new QHBoxLayout();
	auto *lblPreviewTitle = new QLabel(QString::fromUtf8("المعاينة النهائية:"), groupTitleDetails_);
	rowPreview->addWidget(lblPreviewTitle);

	lblFullPreview_ = new QLabel(groupTitleDetails_);
	lblFullPreview_->setObjectName("lblFullPreview");
	lblFullPreview_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	rowPreview->addWidget(lblFullPreview_, 1);
	detailsLayout->addLayout(rowPreview);

	// Row 4: Font settings
	auto *fontLayout = new QHBoxLayout();
	fontLayout->setSpacing(8);

	auto *lblFont = new QLabel(QString::fromUtf8("الخط:"), groupTitleDetails_);
	fontLayout->addWidget(lblFont);

	comboFont_ = new QFontComboBox(groupTitleDetails_);
	comboFont_->setObjectName("comboFont");
	comboFont_->setWritingSystem(QFontDatabase::Arabic);
	comboFont_->setFixedHeight(28);
	connect(comboFont_, &QFontComboBox::currentFontChanged, this, &TitleManagerDialog::onFontSettingsChanged);
	fontLayout->addWidget(comboFont_, 2);

	chkBold_ = new QCheckBox(QString::fromUtf8("عريض"), groupTitleDetails_);
	chkBold_->setObjectName("chkBold");
	connect(chkBold_, &QCheckBox::toggled, this, &TitleManagerDialog::onFontSettingsChanged);
	fontLayout->addWidget(chkBold_);

	btnBrowseFont_ = new QPushButton(QString::fromUtf8("🔤 استعراض..."), groupTitleDetails_);
	btnBrowseFont_->setFixedHeight(28);
	btnBrowseFont_->setCursor(Qt::PointingHandCursor);
	connect(btnBrowseFont_, &QPushButton::clicked, this, &TitleManagerDialog::onBrowseFontClicked);
	fontLayout->addWidget(btnBrowseFont_);

	detailsLayout->addLayout(fontLayout);

	chkTightBounds_ = new QCheckBox(QString::fromUtf8("إحكام هوامش الخط الذكية (إهمال الفراغات الزائدة بالخط والتمركز الدقيق)"), groupTitleDetails_);
	chkTightBounds_->setObjectName("chkTightBounds");
	chkTightBounds_->setChecked(true);
	chkTightBounds_->setToolTip(QString::fromUtf8("يتجاهل الهوامش والمسافات الرأسية الشاغرة التي يفرضها ملف الخط، مما يضمن توسط جسم الخط وتناسقه تماماً داخل الصندوق"));
	connect(chkTightBounds_, &QCheckBox::toggled, this, &TitleManagerDialog::onFontSettingsChanged);
	detailsLayout->addWidget(chkTightBounds_);

	btnSnapFrame_ = new QPushButton(QString::fromUtf8("📐 ملاءمة الصندوق الذكي لحجم هذا العنوان تماماً"), groupTitleDetails_);
	btnSnapFrame_->setObjectName("btnSnapFrame");
	btnSnapFrame_->setFixedHeight(28);
	btnSnapFrame_->setCursor(Qt::PointingHandCursor);
	btnSnapFrame_->setToolTip(QString::fromUtf8("يقوم بضبط حدود الصندوق الذكي لتلتف حول العنوان الحالي بدقة بالغة على مقاس الخط بالضبط دون أي فراغ زائد"));
	connect(btnSnapFrame_, &QPushButton::clicked, this, [this]() {
		OBSBasic *main = OBSBasic::Get();
		if (!main) return;
		OBSScene scene = main->GetCurrentScene();
		if (!scene) return;
		obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *p) -> bool {
			auto *m = static_cast<OBSBasic *>(p);
			obs_source_t *source = obs_sceneitem_get_source(item);
			if (source && IsManagedTitleSource(source)) {
				m->ResetSmartFrameToSourceSize(item);
			}
			return true;
		}, main);
	});
	detailsLayout->addWidget(btnSnapFrame_);

	// Row 5: Action buttons (Apply & Delete)
	auto *rowActions = new QHBoxLayout();
	rowActions->setSpacing(8);

	btnApply_ = new QPushButton(QString::fromUtf8("✅ تطبيق العنوان المحدد على الشاشة وحفظه"), groupTitleDetails_);
	btnApply_->setObjectName("btnApply");
	btnApply_->setFixedHeight(32);
	btnApply_->setCursor(Qt::PointingHandCursor);
	connect(btnApply_, &QPushButton::clicked, this, &TitleManagerDialog::onApplyClicked);
	rowActions->addWidget(btnApply_, 2);

	btnDelete_ = new QPushButton(QString::fromUtf8("🗑️ حذف العنوان"), groupTitleDetails_);
	btnDelete_->setObjectName("btnDelete");
	btnDelete_->setFixedHeight(32);
	btnDelete_->setCursor(Qt::PointingHandCursor);
	connect(btnDelete_, &QPushButton::clicked, this, &TitleManagerDialog::onDeleteTitleClicked);
	rowActions->addWidget(btnDelete_, 1);

	detailsLayout->addLayout(rowActions);

	layout->addWidget(groupTitleDetails_);

	// Add New Title Section
	auto *addBox = new QGroupBox(QString::fromUtf8("➕ إضافة عنوان أو سلسلة جديدة للقائمة:"), this);
	addBox->setObjectName("addBox");
	addBox->setLayoutDirection(Qt::RightToLeft);
	auto *addLayout = new QVBoxLayout(addBox);
	addLayout->setContentsMargins(10, 10, 10, 10);
	addLayout->setSpacing(6);

	auto *addTextRow = new QHBoxLayout();
	txtNewTitle_ = new QLineEdit(addBox);
	txtNewTitle_->setObjectName("txtNewTitle");
	txtNewTitle_->setPlaceholderText(QString::fromUtf8("اكتب اسم العنوان أو السلسلة الجديدة هنا..."));
	txtNewTitle_->setFixedHeight(30);
	connect(txtNewTitle_, &QLineEdit::returnPressed, this, &TitleManagerDialog::onAddTitleClicked);
	addTextRow->addWidget(txtNewTitle_, 2);

	chkNewHasNumber_ = new QCheckBox(QString::fromUtf8("سلسلة مرقمة"), addBox);
	chkNewHasNumber_->setChecked(true);
	addTextRow->addWidget(chkNewHasNumber_);

	auto *lblNewNum = new QLabel(QString::fromUtf8("رقم الدرس:"), addBox);
	addTextRow->addWidget(lblNewNum);

	spinNewLesson_ = new QSpinBox(addBox);
	spinNewLesson_->setRange(1, 99999);
	spinNewLesson_->setValue(1);
	spinNewLesson_->setFixedHeight(30);
	spinNewLesson_->setFixedWidth(70);
	addTextRow->addWidget(spinNewLesson_);

	btnAdd_ = new QPushButton(QString::fromUtf8("➕ إضافة وحفظ"), addBox);
	btnAdd_->setObjectName("btnAdd");
	btnAdd_->setFixedHeight(30);
	btnAdd_->setCursor(Qt::PointingHandCursor);
	connect(btnAdd_, &QPushButton::clicked, this, &TitleManagerDialog::onAddTitleClicked);
	addTextRow->addWidget(btnAdd_);

	addLayout->addLayout(addTextRow);

	btnAddImageTitle_ = new QPushButton(QString::fromUtf8("🖼️ رفع صورة واستخدامها عنواناً"), addBox);
	btnAddImageTitle_->setObjectName("btnAddImageTitle");
	btnAddImageTitle_->setFixedHeight(30);
	btnAddImageTitle_->setCursor(Qt::PointingHandCursor);
	btnAddImageTitle_->setToolTip(QString::fromUtf8("ينسخ الصورة إلى مجلد أصول المشروع ويضيفها إلى المشهد الحالي كعنوان قابل للتحريك والتحجيم"));
	connect(btnAddImageTitle_, &QPushButton::clicked, this, &TitleManagerDialog::onAddImageTitleClicked);
	addLayout->addWidget(btnAddImageTitle_);

	auto *addFontRow = new QHBoxLayout();
	auto *lblNewFont = new QLabel(QString::fromUtf8("الخط:"), addBox);
	addFontRow->addWidget(lblNewFont);

	comboNewFont_ = new QFontComboBox(addBox);
	comboNewFont_->setWritingSystem(QFontDatabase::Arabic);
	comboNewFont_->setFixedHeight(26);
	addFontRow->addWidget(comboNewFont_, 2);

	chkNewBold_ = new QCheckBox(QString::fromUtf8("عريض"), addBox);
	addFontRow->addWidget(chkNewBold_);
	addLayout->addLayout(addFontRow);

	layout->addWidget(addBox);

	// Temporary Title Section
	groupTemp_ = new QGroupBox(QString::fromUtf8("⚡ عنوان مؤقت (لجلسة البث الحالية فقط — يختفي عند إغلاق البرنامج):"), this);
	groupTemp_->setObjectName("groupTemp");
	groupTemp_->setLayoutDirection(Qt::RightToLeft);
	auto *tempLayout = new QVBoxLayout(groupTemp_);
	tempLayout->setContentsMargins(10, 10, 10, 10);
	tempLayout->setSpacing(8);

	auto *tempInputRow = new QHBoxLayout();
	tempInputRow->setSpacing(6);

	txtTempTitle_ = new QLineEdit(groupTemp_);
	txtTempTitle_->setObjectName("txtTempTitle");
	txtTempTitle_->setPlaceholderText(QString::fromUtf8("اكتب العنوان المؤقت هنا..."));
	txtTempTitle_->setFixedHeight(30);
	connect(txtTempTitle_, &QLineEdit::returnPressed, this, &TitleManagerDialog::onApplyTempClicked);
	tempInputRow->addWidget(txtTempTitle_, 2);

	btnApplyTemp_ = new QPushButton(QString::fromUtf8("⚡ تطبيق مؤقت"), groupTemp_);
	btnApplyTemp_->setObjectName("btnApplyTemp");
	btnApplyTemp_->setFixedHeight(30);
	btnApplyTemp_->setCursor(Qt::PointingHandCursor);
	connect(btnApplyTemp_, &QPushButton::clicked, this, &TitleManagerDialog::onApplyTempClicked);
	tempInputRow->addWidget(btnApplyTemp_, 1);

	btnCancelTemp_ = new QPushButton(QString::fromUtf8("🗑️ إلغاء المؤقت"), groupTemp_);
	btnCancelTemp_->setObjectName("btnCancelTemp");
	btnCancelTemp_->setFixedHeight(30);
	btnCancelTemp_->setCursor(Qt::PointingHandCursor);
	connect(btnCancelTemp_, &QPushButton::clicked, this, &TitleManagerDialog::onCancelTempClicked);
	tempInputRow->addWidget(btnCancelTemp_, 1);

	tempLayout->addLayout(tempInputRow);

	// Font styling controls for temporary title
	auto *tempFontRow = new QHBoxLayout();
	tempFontRow->setSpacing(8);

	auto *lblTempFont = new QLabel(QString::fromUtf8("الخط:"), groupTemp_);
	tempFontRow->addWidget(lblTempFont);

	comboTempFont_ = new QFontComboBox(groupTemp_);
	comboTempFont_->setObjectName("comboTempFont");
	comboTempFont_->setWritingSystem(QFontDatabase::Arabic);
	comboTempFont_->setFixedHeight(28);
	tempFontRow->addWidget(comboTempFont_, 2);

	chkTempBold_ = new QCheckBox(QString::fromUtf8("عريض (Bold)"), groupTemp_);
	chkTempBold_->setObjectName("chkTempBold");
	chkTempBold_->setCursor(Qt::PointingHandCursor);
	tempFontRow->addWidget(chkTempBold_);

	btnBrowseTempFont_ = new QPushButton(QString::fromUtf8("🎨 تصفح الخطوط"), groupTemp_);
	btnBrowseTempFont_->setObjectName("btnBrowseTempFont");
	btnBrowseTempFont_->setFixedHeight(28);
	btnBrowseTempFont_->setCursor(Qt::PointingHandCursor);
	connect(btnBrowseTempFont_, &QPushButton::clicked, this, &TitleManagerDialog::onBrowseTempFontClicked);
	tempFontRow->addWidget(btnBrowseTempFont_);

	tempLayout->addLayout(tempFontRow);

	// Load persistent temp font settings
	QString savedTempFont; int savedTempSize = 120; bool savedTempBold = false;
	GetTemporaryTitleFont(savedTempFont, savedTempSize, savedTempBold);
	comboTempFont_->setCurrentFont(QFont(savedTempFont));
	chkTempBold_->setChecked(savedTempBold);

	connect(comboTempFont_, &QFontComboBox::currentFontChanged, this, &TitleManagerDialog::onTempFontSettingsChanged);
	connect(chkTempBold_, &QCheckBox::toggled, this, &TitleManagerDialog::onTempFontSettingsChanged);

	layout->addWidget(groupTemp_);

	// Bottom Row
	auto *bottomRow = new QHBoxLayout();
	bottomRow->addStretch();

	btnClose_ = new QPushButton(QString::fromUtf8("إغلاق"), this);
	btnClose_->setObjectName("btnClose");
	btnClose_->setFixedHeight(32);
	btnClose_->setFixedWidth(110);
	btnClose_->setCursor(Qt::PointingHandCursor);
	connect(btnClose_, &QPushButton::clicked, this, &QDialog::accept);
	bottomRow->addWidget(btnClose_);

	layout->addLayout(bottomRow);
}

void TitleManagerDialog::applyStylesheet()
{
	// Adhere completely to HK Royal theme tokens; no hardcoded non-compliant colors
	setStyleSheet(QString());
}

void TitleManagerDialog::updateFullPreview()
{
	QString base = txtEditBaseTitle_->text().trimmed();
	bool hasNum = chkHasNumber_->isChecked();
	int num = spinLessonNum_->value();
	QString sep = comboSeparator_->currentData().toString();
	if (sep.isEmpty()) sep = " / ";

	QString full;
	if (hasNum) {
		full = QString("%1%2%3").arg(base, sep, QString::number(num)).trimmed();
	} else {
		full = base;
	}

	lblFullPreview_->setText(full.isEmpty() ? QString::fromUtf8("(لا يوجد عنوان)") : full);
}

void TitleManagerDialog::refreshList()
{
	if (!comboTitles_) return;
	comboTitles_->blockSignals(true);
	comboTitles_->clear();
	int activeIndex = -1;
	for (int i = 0; i < titlesData_.size(); ++i) {
		const auto &entry = titlesData_[i];
		QString full = entry.fullTitle();
		bool isActive = (full == activeTitle_ || entry.text == activeTitle_);
		QString fontInfo = entry.isImage ? QString() : QString(" [%1%2]").arg(entry.fontFace).arg(entry.isBold ? QString::fromUtf8(" | عريض") : "");

		QString prefix;
		if (entry.isImage) {
			prefix = QString::fromUtf8("🖼️ ");
		} else if (entry.hasNumber) {
			prefix = QString::fromUtf8("📚 ");
		} else {
			prefix = QString::fromUtf8("🏷️ ");
		}

		QString display;
		if (isActive) {
			display = QString::fromUtf8("⭐ ") + prefix + full + fontInfo + QString::fromUtf8("  (معروض حالياً)");
		} else {
			display = prefix + full + fontInfo;
		}

		comboTitles_->addItem(display, entry.text);
		if (isActive) {
			activeIndex = i;
		}
	}
	if (activeIndex >= 0) {
		comboTitles_->setCurrentIndex(activeIndex);
	} else if (comboTitles_->count() > 0) {
		comboTitles_->setCurrentIndex(0);
	}
	comboTitles_->blockSignals(false);

	if (lblCurrentActive_) {
		lblCurrentActive_->setText(QString::fromUtf8("العنوان المعروض حالياً: ") + activeTitle_);
	}

	// Populate controls for active item
	auto *currentEntry = currentSelectedEntry();
	if (currentEntry) {
		txtEditBaseTitle_->blockSignals(true);
		chkHasNumber_->blockSignals(true);
		spinLessonNum_->blockSignals(true);
		comboSeparator_->blockSignals(true);
		comboFont_->blockSignals(true);
		chkBold_->blockSignals(true);

		txtEditBaseTitle_->setText(currentEntry->baseTitle.isEmpty() ? currentEntry->text : currentEntry->baseTitle);
		chkHasNumber_->setChecked(currentEntry->hasNumber);
		spinLessonNum_->setValue(currentEntry->lessonNumber);
		spinLessonNum_->setEnabled(currentEntry->hasNumber);
		btnIncLesson_->setEnabled(currentEntry->hasNumber);
		btnDecLesson_->setEnabled(currentEntry->hasNumber);

		int sepIdx = comboSeparator_->findData(currentEntry->separator);
		if (sepIdx >= 0) {
			comboSeparator_->setCurrentIndex(sepIdx);
		}

		comboFont_->setCurrentFont(QFont(currentEntry->fontFace));
		chkBold_->setChecked(currentEntry->isBold);
		const bool textTitle = !currentEntry->isImage;
		txtEditBaseTitle_->setEnabled(textTitle);
		chkHasNumber_->setEnabled(textTitle);
		spinLessonNum_->setEnabled(textTitle && currentEntry->hasNumber);
		btnIncLesson_->setEnabled(textTitle && currentEntry->hasNumber);
		btnDecLesson_->setEnabled(textTitle && currentEntry->hasNumber);
		comboSeparator_->setEnabled(textTitle);
		comboFont_->setEnabled(textTitle);
		chkBold_->setEnabled(textTitle);
		btnBrowseFont_->setEnabled(textTitle);
		chkTightBounds_->setEnabled(textTitle);

		txtEditBaseTitle_->blockSignals(false);
		chkHasNumber_->blockSignals(false);
		spinLessonNum_->blockSignals(false);
		comboSeparator_->blockSignals(false);
		comboFont_->blockSignals(false);
		chkBold_->blockSignals(false);

		if (currentEntry->isImage)
			lblFullPreview_->setText(QString::fromUtf8("صورة العنوان: ") + QFileInfo(currentEntry->imagePath).fileName());
		else
			updateFullPreview();
	}
}

void TitleManagerDialog::onTitleSelected(int index)
{
	if (index < 0 || !comboTitles_) return;
	RestoreTemporaryTitleFromOBS();
	QString titleKey = comboTitles_->itemData(index).toString();

	auto *entry = findEntry(titleKey);
	if (!entry) return;

	activeTitle_ = entry->fullTitle();

	txtEditBaseTitle_->blockSignals(true);
	chkHasNumber_->blockSignals(true);
	spinLessonNum_->blockSignals(true);
	comboSeparator_->blockSignals(true);
	comboFont_->blockSignals(true);
	chkBold_->blockSignals(true);

	txtEditBaseTitle_->setText(entry->baseTitle.isEmpty() ? entry->text : entry->baseTitle);
	chkHasNumber_->setChecked(entry->hasNumber);
	spinLessonNum_->setValue(entry->lessonNumber);
	spinLessonNum_->setEnabled(entry->hasNumber);
	btnIncLesson_->setEnabled(entry->hasNumber);
	btnDecLesson_->setEnabled(entry->hasNumber);

	int sepIdx = comboSeparator_->findData(entry->separator);
	if (sepIdx >= 0) {
		comboSeparator_->setCurrentIndex(sepIdx);
	}

	comboFont_->setCurrentFont(QFont(entry->fontFace));
	chkBold_->setChecked(entry->isBold);
	const bool textTitle = !entry->isImage;
	txtEditBaseTitle_->setEnabled(textTitle);
	chkHasNumber_->setEnabled(textTitle);
	spinLessonNum_->setEnabled(textTitle && entry->hasNumber);
	btnIncLesson_->setEnabled(textTitle && entry->hasNumber);
	btnDecLesson_->setEnabled(textTitle && entry->hasNumber);
	comboSeparator_->setEnabled(textTitle);
	comboFont_->setEnabled(textTitle);
	chkBold_->setEnabled(textTitle);
	btnBrowseFont_->setEnabled(textTitle);
	chkTightBounds_->setEnabled(textTitle);

	txtEditBaseTitle_->blockSignals(false);
	chkHasNumber_->blockSignals(false);
	spinLessonNum_->blockSignals(false);
	comboSeparator_->blockSignals(false);
	comboFont_->blockSignals(false);
	chkBold_->blockSignals(false);

	if (entry->isImage)
		lblFullPreview_->setText(QString::fromUtf8("صورة العنوان: ") + QFileInfo(entry->imagePath).fileName());
	else
		updateFullPreview();

	ApplyTitleToOBS(entry->fullTitle(), entry->fontFace, entry->fontSize, entry->isBold,
	                chkTightBounds_ ? chkTightBounds_->isChecked() : true);
	SaveTitlesData(titlesData_, activeTitle_);
	if (lblCurrentActive_) {
		lblCurrentActive_->setText(QString::fromUtf8("العنوان المعروض حالياً: ") + activeTitle_);
	}
	emit titleChanged(activeTitle_);
}

void TitleManagerDialog::onLessonNumberChanged()
{
	auto *entry = currentSelectedEntry();
	if (!entry || entry->isImage) return;

	bool hasNum = chkHasNumber_->isChecked();
	spinLessonNum_->setEnabled(hasNum);
	btnIncLesson_->setEnabled(hasNum);
	btnDecLesson_->setEnabled(hasNum);

	entry->baseTitle = txtEditBaseTitle_->text().trimmed();
	entry->hasNumber = hasNum;
	entry->lessonNumber = spinLessonNum_->value();
	entry->separator = comboSeparator_->currentData().toString();
	entry->text = entry->fullTitle();

	updateFullPreview();

	RestoreTemporaryTitleFromOBS();
	ApplyTitleToOBS(entry->text, entry->fontFace, entry->fontSize, entry->isBold,
	                chkTightBounds_ ? chkTightBounds_->isChecked() : true);

	activeTitle_ = entry->text;
	if (comboTitles_ && comboTitles_->currentIndex() >= 0) {
		int idx = comboTitles_->currentIndex();
		comboTitles_->setItemData(idx, entry->text);
		QString fontInfo = QString(" [%1%2]").arg(entry->fontFace).arg(entry->isBold ? QString::fromUtf8(" | عريض") : "");
		QString prefix = entry->hasNumber ? QString::fromUtf8("📚 ") : QString::fromUtf8("🏷️ ");
		comboTitles_->setItemText(idx, QString::fromUtf8("⭐ ") + prefix + entry->text + fontInfo);
	}

	SaveTitlesData(titlesData_, activeTitle_);
	if (lblCurrentActive_) {
		lblCurrentActive_->setText(QString::fromUtf8("العنوان المعروض حالياً: ") + activeTitle_);
	}

	emit titleChanged(activeTitle_);
}

void TitleManagerDialog::onIncrementLessonClicked()
{
	spinLessonNum_->setValue(spinLessonNum_->value() + 1);
}

void TitleManagerDialog::onDecrementLessonClicked()
{
	if (spinLessonNum_->value() > 1) {
		spinLessonNum_->setValue(spinLessonNum_->value() - 1);
	}
}

void TitleManagerDialog::onFontSettingsChanged()
{
	auto *entry = currentSelectedEntry();
	if (!entry || entry->isImage) return;

	entry->fontFace = comboFont_->currentFont().family();
	entry->isBold = chkBold_->isChecked();
	RestoreTemporaryTitleFromOBS();

	ApplyTitleToOBS(entry->fullTitle(), entry->fontFace, entry->fontSize, entry->isBold,
	                chkTightBounds_ ? chkTightBounds_->isChecked() : true);
	SaveTitlesData(titlesData_, activeTitle_);

	if (comboTitles_ && comboTitles_->currentIndex() >= 0) {
		QString fontInfo = QString(" [%1%2]").arg(entry->fontFace).arg(entry->isBold ? QString::fromUtf8(" | عريض") : "");
		QString prefix = entry->hasNumber ? QString::fromUtf8("📚 ") : QString::fromUtf8("🏷️ ");
		bool isActive = (entry->fullTitle() == activeTitle_ || entry->text == activeTitle_);
		QString disp = (isActive ? QString::fromUtf8("⭐ ") : "") + prefix + entry->fullTitle() + fontInfo;
		comboTitles_->setItemText(comboTitles_->currentIndex(), disp);
	}
}

static bool ShowArabicFontDialog(QWidget *parent, const QFont &initialFont, QFont &outFont)
{
	QFontDialog dlg(initialFont, parent);
	dlg.setWindowTitle(QString::fromUtf8("اختيار نوع الخط للعنوان"));
	dlg.setOption(QFontDialog::DontUseNativeDialog, true);
	dlg.setLayoutDirection(Qt::RightToLeft);

	for (auto *lbl : dlg.findChildren<QLabel *>()) {
		QString t = lbl->text().trimmed();
		if (t.contains("Font:", Qt::CaseInsensitive) || t == "&Font:") {
			lbl->setText(QString::fromUtf8("الخط:"));
		} else if (t.contains("Font style:", Qt::CaseInsensitive) || t == "Font &style:") {
			lbl->setText(QString::fromUtf8("نمط الخط:"));
		} else if (t.contains("Size:", Qt::CaseInsensitive) || t == "&Size:" || t.contains("الحجم", Qt::CaseInsensitive)) {
			lbl->hide();
			if (lbl->buddy()) lbl->buddy()->hide();
		} else if (t.contains("Writing System", Qt::CaseInsensitive)) {
			lbl->setText(QString::fromUtf8("نظام الكتابة:"));
		} else if (t.contains("Sample", Qt::CaseInsensitive)) {
			lbl->setText(QString::fromUtf8("معاينة الخط:"));
		}
	}

	for (auto *w : dlg.findChildren<QWidget *>()) {
		if (w->objectName().contains("size", Qt::CaseInsensitive)) {
			w->hide();
		}
	}

	for (auto *gb : dlg.findChildren<QGroupBox *>()) {
		QString t = gb->title().trimmed();
		if (t.contains("Effects", Qt::CaseInsensitive)) {
			gb->setTitle(QString::fromUtf8("تأثيرات"));
		} else if (t.contains("Sample", Qt::CaseInsensitive)) {
			gb->setTitle(QString::fromUtf8("معاينة الخط"));
		}
	}

	for (auto *chk : dlg.findChildren<QCheckBox *>()) {
		QString t = chk->text().trimmed();
		if (t.contains("Strikeout", Qt::CaseInsensitive)) {
			chk->setText(QString::fromUtf8("يتوسطه خط"));
		} else if (t.contains("Underline", Qt::CaseInsensitive)) {
			chk->setText(QString::fromUtf8("تسطير"));
		}
	}

	for (auto *btnBox : dlg.findChildren<QDialogButtonBox *>()) {
		if (auto *btn = btnBox->button(QDialogButtonBox::Ok)) {
			btn->setText(QString::fromUtf8("موافق"));
		}
		if (auto *btn = btnBox->button(QDialogButtonBox::Cancel)) {
			btn->setText(QString::fromUtf8("إلغاء الأمر"));
		}
	}

	// تحديد حقل المعاينة (Sample) وضبط النص ليكون 'بسم الله' دائماً مع ملاءمة الحجم داخل الإطار
	QLineEdit *sampleEdit = nullptr;
	for (auto *gb : dlg.findChildren<QGroupBox *>()) {
		if (gb->title().contains(QString::fromUtf8("معاينة")) || gb->title().contains("Sample", Qt::CaseInsensitive)) {
			sampleEdit = gb->findChild<QLineEdit *>();
			break;
		}
	}
	if (!sampleEdit) {
		for (auto *le : dlg.findChildren<QLineEdit *>()) {
			if (le->isReadOnly() || le->text().contains("AaBb") || le->text().contains("Sample") || le->text().isEmpty()) {
				sampleEdit = le;
				break;
			}
		}
	}

	auto updateSampleFitted = [sampleEdit, &dlg]() {
		if (!sampleEdit) return;
		const QString sampleText = QString::fromUtf8("بسم الله");
		sampleEdit->blockSignals(true);
		sampleEdit->setText(sampleText);
		sampleEdit->setAlignment(Qt::AlignCenter);

		QFont curFont = dlg.currentFont();
		QFont fitFont = curFont;

		int boxW = sampleEdit->width();
		int boxH = sampleEdit->height();
		if (boxW <= 0) boxW = 280;
		if (boxH <= 0) boxH = 65;
		int maxW = qMax(boxW - 24, 60);
		int maxH = qMax(boxH - 16, 24);

		int curPt = curFont.pointSize() > 0 ? curFont.pointSize() : (curFont.pixelSize() > 0 ? curFont.pixelSize() : 36);
		while (curPt > 10) {
			fitFont.setPointSize(curPt);
			QFontMetrics fm(fitFont);
			if (fm.horizontalAdvance(sampleText) <= maxW && fm.height() <= maxH) {
				break;
			}
			curPt = qMax(curPt - 2, 8);
		}
		fitFont.setPointSize(curPt);
		sampleEdit->setFont(fitFont);
		sampleEdit->blockSignals(false);
	};

	if (sampleEdit) {
		sampleEdit->setReadOnly(true);
		sampleEdit->setAlignment(Qt::AlignCenter);
	}

	QObject::connect(&dlg, &QFontDialog::currentFontChanged, &dlg, [updateSampleFitted](const QFont &) {
		QTimer::singleShot(0, [updateSampleFitted]() {
			updateSampleFitted();
		});
	});

	QTimer::singleShot(0, [updateSampleFitted]() {
		updateSampleFitted();
	});

	if (dlg.exec() == QDialog::Accepted) {
		outFont = dlg.selectedFont();
		return true;
	}
	return false;
}

void TitleManagerDialog::onBrowseFontClicked()
{
	auto *entry = currentSelectedEntry();
	if (!entry) return;

	QFont initialFont(entry->fontFace, entry->fontSize);
	initialFont.setBold(entry->isBold);

	QFont chosen;
	if (ShowArabicFontDialog(this, initialFont, chosen)) {
		comboFont_->setCurrentFont(chosen);
		chkBold_->setChecked(chosen.bold());
		onFontSettingsChanged();
	}
}

void TitleManagerDialog::onBrowseTempFontClicked()
{
	QFont initial = comboTempFont_ ? comboTempFont_->currentFont() : QFont("zain mob-long300");
	if (chkTempBold_) {
		initial.setBold(chkTempBold_->isChecked());
	}

	QFont chosen;
	if (ShowArabicFontDialog(this, initial, chosen)) {
		if (comboTempFont_) comboTempFont_->setCurrentFont(chosen);
		if (chkTempBold_) chkTempBold_->setChecked(chosen.bold());
		onTempFontSettingsChanged();
	}
}

void TitleManagerDialog::onTempFontSettingsChanged()
{
	if (!comboTempFont_ || !chkTempBold_) return;
	QString font = comboTempFont_->currentFont().family();
	bool bold = chkTempBold_->isChecked();
	SaveTemporaryTitleFont(font, 120, bold);
}

void TitleManagerDialog::onApplyClicked()
{
	if (!comboTitles_ || comboTitles_->currentIndex() < 0) return;
	onTitleSelected(comboTitles_->currentIndex());
	refreshList();
}

void TitleManagerDialog::onAddTitleClicked()
{
	QString newTitle = txtNewTitle_->text().trimmed();
	if (newTitle.isEmpty()) {
		return;
	}

	TitleEntry entry;
	entry.fontFace = comboNewFont_->currentFont().family();
	entry.fontSize = 120;
	entry.isBold = chkNewBold_->isChecked();
	entry.hasNumber = chkNewHasNumber_->isChecked();
	entry.lessonNumber = spinNewLesson_->value();
	entry.separator = " / ";
	entry.baseTitle = newTitle;
	entry.text = entry.fullTitle();

	RestoreTemporaryTitleFromOBS();

	auto *existing = findEntry(entry.text);
	if (!existing) {
		titlesData_.append(entry);
	} else {
		*existing = entry;
	}

	activeTitle_ = entry.text;
	ApplyTitleToOBS(entry.text, entry.fontFace, entry.fontSize, entry.isBold,
	                chkTightBounds_ ? chkTightBounds_->isChecked() : true);
	SaveTitlesData(titlesData_, activeTitle_);

	txtNewTitle_->clear();
	refreshList();
	emit titleChanged(activeTitle_);
}

void TitleManagerDialog::onAddImageTitleClicked()
{
	const QString filePath = QFileDialog::getOpenFileName(
		this, QString::fromUtf8("اختيار صورة العنوان"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
		QString::fromUtf8("الصور (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;كل الملفات (*.*)"));
	if (filePath.isEmpty()) return;

	const QString targetDir = QString::fromStdString(OBSWorkspaceManager::GetStyleFilePath("assets/images"));
	if (!QDir().mkpath(targetDir)) {
		QMessageBox::warning(this, QString::fromUtf8("تعذر حفظ الصورة"), QString::fromUtf8("لم أتمكن من إنشاء مجلد صور العناوين داخل أصول المشروع."));
		return;
	}

	QFileInfo sourceInfo(filePath);
	QString storedPath = QDir(targetDir).filePath(sourceInfo.fileName());
	if (QFileInfo(filePath).canonicalFilePath() != QFileInfo(storedPath).canonicalFilePath()) {
		int suffix = 1;
		while (QFile::exists(storedPath)) {
			storedPath = QDir(targetDir).filePath(
				QString("%1_%2.%3").arg(sourceInfo.completeBaseName()).arg(suffix++).arg(sourceInfo.suffix()));
		}
		if (!QFile::copy(filePath, storedPath)) {
			QMessageBox::warning(this, QString::fromUtf8("تعذر نسخ الصورة"), QString::fromUtf8("لم أتمكن من نسخ الصورة إلى مجلد أصول المشروع."));
			return;
		}
	}

	QString label = sourceInfo.completeBaseName().trimmed();
	if (label.isEmpty()) label = QString::fromUtf8("عنوان مصور");
	QString uniqueLabel = label;
	int index = 2;
	while (findEntry(uniqueLabel))
		uniqueLabel = QString::fromUtf8("%1 (صورة %2)").arg(label).arg(index++);

	TitleEntry entry;
	entry.isImage = true;
	entry.imagePath = storedPath;
	entry.baseTitle = uniqueLabel;
	entry.text = uniqueLabel;
	entry.hasNumber = false;

	RestoreTemporaryTitleFromOBS();
	titlesData_.append(entry);
	activeTitle_ = entry.text;
	if (!SaveTitlesData(titlesData_, activeTitle_)) {
		QMessageBox::warning(this, QString::fromUtf8("تعذر حفظ العنوان"), QString::fromUtf8("تم نسخ الصورة، لكن تعذر حفظها في قائمة العناوين."));
		return;
	}
	ApplyTitleToOBS(entry.text);
	refreshList();
	txtNewTitle_->clear();
	emit titleChanged(activeTitle_);
}

void TitleManagerDialog::onDeleteTitleClicked()
{
	auto *entry = currentSelectedEntry();
	if (!entry) {
		QMessageBox::information(this, QString::fromUtf8("تنبيه"), QString::fromUtf8("يرجى تحديد العنوان الذي تريد حذفه من القائمة."));
		return;
	}

	QString titleToDelete = entry->text;

	if (titlesData_.size() <= 1) {
		QMessageBox::warning(this, QString::fromUtf8("تنبيه"), QString::fromUtf8("لا يمكن حذف جميع العناوين، يجب أن يبقى عنوان واحد على الأقل."));
		return;
	}

	QMessageBox::StandardButton reply = QMessageBox::question(
		this,
		QString::fromUtf8("تأكيد الحذف"),
		QString::fromUtf8("هل أنت متأكد من حذف العنوان التالي نهائياً من القائمة؟\n\n« %1 »").arg(titleToDelete),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No);

	if (reply != QMessageBox::Yes) {
		return;
	}
	RestoreTemporaryTitleFromOBS();

	for (int i = 0; i < titlesData_.size(); ++i) {
		if (titlesData_[i].text == titleToDelete || titlesData_[i].fullTitle() == titleToDelete) {
			titlesData_.removeAt(i);
			break;
		}
	}

	if (activeTitle_ == titleToDelete) {
		if (!titlesData_.isEmpty()) {
			activeTitle_ = titlesData_.first().fullTitle();
			ApplyTitleToOBS(titlesData_.first().fullTitle(), titlesData_.first().fontFace, titlesData_.first().fontSize, titlesData_.first().isBold);
		}
	}

	SaveTitlesData(titlesData_, activeTitle_);
	refreshList();
	emit titleChanged(activeTitle_);
}

void TitleManagerDialog::onApplyTempClicked()
{
	QString tempText = txtTempTitle_->text().trimmed();
	if (tempText.isEmpty()) {
		QMessageBox::information(this, QString::fromUtf8("تنبيه"), QString::fromUtf8("يرجى كتابة نص العنوان المؤقت أولاً."));
		return;
	}

	QString font = comboTempFont_ ? comboTempFont_->currentFont().family() : QString();
	bool bold = chkTempBold_ ? chkTempBold_->isChecked() : false;

	SaveTemporaryTitleFont(font, 120, bold);
	ApplyTemporaryTitleToOBS(tempText, font, 120, bold);
	lblCurrentActive_->setText(QString::fromUtf8("العنوان المعروض حالياً (مؤقت): ") + tempText);
	emit titleChanged(tempText);

	QMessageBox::information(this, QString::fromUtf8("تم تفعيل العنوان المؤقت"),
	                         QString::fromUtf8("تم تطبيق العنوان المؤقت على شاشة البث بنجاح.\n\nملاحظة: هذا العنوان لن يُحفظ في قاعدة البيانات، وعند إعادة تشغيل البرنامج سيتم استعادة العنوان الأساسي تلقائياً."));
}

void TitleManagerDialog::onCancelTempClicked()
{
	RestoreTemporaryTitleFromOBS();

	lblCurrentActive_->setText(QString::fromUtf8("العنوان المعروض حالياً: ") + activeTitle_);
	txtTempTitle_->clear();
	emit titleChanged(activeTitle_);
}

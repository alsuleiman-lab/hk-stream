#include "WebSourceCodeDialog.hpp"
#include "workspaces/WorkspaceModel.hpp"
#include "widgets/OBSBasic.hpp"
#include "widgets/OBSQTDisplay.hpp"
#include <graphics/graphics.h>
#include <obs.h>
#include <obs-frontend-api.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QGroupBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QMessageBox>
#include <QTimer>
#include <QSignalBlocker>
#include <algorithm>

WebSourceCodeDialog::WebSourceCodeDialog(obs_source_t *source, OBSBasic *main, QWidget *parent)
	: QDialog(parent), source_(source), main_(main)
{
	const char *name = source ? obs_source_get_name(source) : "مصدر ويب";
	setWindowTitle(QString("🌐 محرر كود الويب والمعاينة الحية - [%1]").arg(QString::fromUtf8(name)));
	setLayoutDirection(Qt::RightToLeft);
	resize(1150, 720);

	buildUI();
	loadSourceCode();

	if (source_) {
		obs_source_inc_showing(source_);
	}

	auto addDrawCb = [this](OBSQTDisplay *) {
		if (previewDisplay_ && previewDisplay_->GetDisplay()) {
			obs_display_add_draw_callback(previewDisplay_->GetDisplay(), WebSourceCodeDialog::DrawPreview, this);
		}
	};
	connect(previewDisplay_, &OBSQTDisplay::DisplayCreated, this, addDrawCb);
	if (previewDisplay_->GetDisplay()) {
		obs_display_add_draw_callback(previewDisplay_->GetDisplay(), WebSourceCodeDialog::DrawPreview, this);
	}
}

WebSourceCodeDialog::~WebSourceCodeDialog()
{
	if (previewDisplay_ && previewDisplay_->GetDisplay()) {
		obs_display_remove_draw_callback(previewDisplay_->GetDisplay(), WebSourceCodeDialog::DrawPreview, this);
	}
	if (source_) {
		obs_source_dec_showing(source_);
	}
}

void WebSourceCodeDialog::buildUI()
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(14, 12, 14, 12);
	mainLayout->setSpacing(10);

	// Header
	auto *headerRow = new QHBoxLayout();
	auto *lblTitle = new QLabel(this);
	const char *srcName = source_ ? obs_source_get_name(source_) : "";
	lblTitle->setTextFormat(Qt::PlainText);
	lblTitle->setText(QString("محرر كود الويب: %1").arg(QString::fromUtf8(srcName)));
	headerRow->addWidget(lblTitle);

	headerRow->addStretch();

	lblStatus_ = new QLabel("جاهز", this);
	headerRow->addWidget(lblStatus_);

	mainLayout->addLayout(headerRow);

	// Splitter between Code Editor (Right in RTL) and Preview (Left in RTL)
	auto *splitter = new QSplitter(Qt::Horizontal, this);
	splitter->setLayoutDirection(Qt::RightToLeft);
	splitter->setChildrenCollapsible(false);

	// 1. Right Pane: Code Editor and Settings
	auto *editorContainer = new QWidget(splitter);
	auto *ecLayout = new QVBoxLayout(editorContainer);
	ecLayout->setContentsMargins(4, 4, 4, 4);
	ecLayout->setSpacing(8);

	auto *editorToolRow = new QHBoxLayout();
	auto *lblEditorHint = new QLabel("📝 كود HTML / CSS / JavaScript:", editorContainer);
	editorToolRow->addWidget(lblEditorHint);

	editorToolRow->addSpacing(10);

	comboPreset_ = new QComboBox(editorContainer);
	comboPreset_->addItem("📁 كود المصدر الحالي");
	comboPreset_->addItem("⭕ دائرة تفاعلية (دائرة يمين)");
	comboPreset_->addItem("📱 شريط تيكتوك وعناصر التواصل");
	comboPreset_->addItem("⏰ تنبيه الوقت وساعة رقمية");
	comboPreset_->addItem("📅 تاريخ هجري وميلادي");
	comboPreset_->addItem("📄 قالب HTML5 فارغ");
	comboPreset_->setToolTip("اختر قالباً جاهزاً لإدراجه في المحرر");
	connect(comboPreset_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &WebSourceCodeDialog::onPresetChanged);
	editorToolRow->addWidget(comboPreset_);

	editorToolRow->addStretch();

	btnRefresh_ = new QPushButton("🔄 تحديث المعاينة الحية", editorContainer);
	btnRefresh_->setCursor(Qt::PointingHandCursor);
	btnRefresh_->setToolTip("تطبيق الكود فوراً ورؤية النتيجة في شاشة المعاينة");
	connect(btnRefresh_, &QPushButton::clicked, this, &WebSourceCodeDialog::onRefreshPreview);
	editorToolRow->addWidget(btnRefresh_);

	ecLayout->addLayout(editorToolRow);

	codeEditor_ = new QPlainTextEdit(editorContainer);
	codeEditor_->setObjectName("codeEditor");
	codeEditor_->setLayoutDirection(Qt::LeftToRight);
	QFont f = codeEditor_->font();
	f.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
	codeEditor_->setFont(f);
	codeEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
	codeEditor_->setPlaceholderText("أدخل كود HTML / CSS / JS هنا مباشرة دون الحاجة لملفات خارجية...");
	connect(codeEditor_, &QPlainTextEdit::textChanged, this, &WebSourceCodeDialog::onCodeChanged);
	ecLayout->addWidget(codeEditor_, 1);

	// Settings Box under editor
	auto *settingsGroup = new QGroupBox("⚙️ أبعاد العرض (Width × Height)", editorContainer);
	auto *sgLayout = new QHBoxLayout(settingsGroup);
	sgLayout->setContentsMargins(10, 10, 10, 10);
	sgLayout->setSpacing(14);

	sgLayout->addWidget(new QLabel("العرض (Width):", settingsGroup));
	spinWidth_ = new QSpinBox(settingsGroup);
	spinWidth_->setRange(10, 7680);
	spinWidth_->setValue(1920);
	spinWidth_->setSuffix(" px");
	connect(spinWidth_, QOverload<int>::of(&QSpinBox::valueChanged), this, &WebSourceCodeDialog::onRefreshPreview);
	sgLayout->addWidget(spinWidth_);

	sgLayout->addWidget(new QLabel("الارتفاع (Height):", settingsGroup));
	spinHeight_ = new QSpinBox(settingsGroup);
	spinHeight_->setRange(10, 4320);
	spinHeight_->setValue(1080);
	spinHeight_->setSuffix(" px");
	connect(spinHeight_, QOverload<int>::of(&QSpinBox::valueChanged), this, &WebSourceCodeDialog::onRefreshPreview);
	sgLayout->addWidget(spinHeight_);

	sgLayout->addStretch();
	ecLayout->addWidget(settingsGroup);

	splitter->addWidget(editorContainer);

	// 2. Left Pane: Live OBS Direct3D Preview
	auto *previewContainer = new QWidget(splitter);
	auto *pcLayout = new QVBoxLayout(previewContainer);
	pcLayout->setContentsMargins(4, 4, 4, 4);
	pcLayout->setSpacing(8);

	auto *lblPreviewTitle = new QLabel("👁️ المعاينة الحية المباشرة (مطابقة لما يظهر على الشاشة):", previewContainer);
	pcLayout->addWidget(lblPreviewTitle);

	previewDisplay_ = new OBSQTDisplay(previewContainer);
	previewDisplay_->setObjectName("webPreviewDisplay");
	previewDisplay_->setMinimumSize(360, 260);
	pcLayout->addWidget(previewDisplay_, 1);

	auto *lblPreviewHelp = new QLabel("تظهر نتيجة الكود هنا بعد الضغط على تحديث المعاينة.", previewContainer);
	lblPreviewHelp->setWordWrap(true);
	pcLayout->addWidget(lblPreviewHelp);

	splitter->addWidget(previewContainer);

	splitter->setSizes({620, 480});
	mainLayout->addWidget(splitter, 1);

	// Bottom Buttons
	auto *bottomRow = new QHBoxLayout();

	btnAdvProps_ = new QPushButton("⚙️ خصائص المتصفح المتقدمة...", this);
	btnAdvProps_->setCursor(Qt::PointingHandCursor);
	connect(btnAdvProps_, &QPushButton::clicked, this, &WebSourceCodeDialog::onOpenAdvancedProperties);
	bottomRow->addWidget(btnAdvProps_);

	bottomRow->addStretch();

	btnCancel_ = new QPushButton("إلغاء", this);
	btnCancel_->setCursor(Qt::PointingHandCursor);
	connect(btnCancel_, &QPushButton::clicked, this, &QDialog::reject);
	bottomRow->addWidget(btnCancel_);

	btnSave_ = new QPushButton("✅ حفظ وتطبيق على الشاشة", this);
	btnSave_->setCursor(Qt::PointingHandCursor);
	connect(btnSave_, &QPushButton::clicked, this, &WebSourceCodeDialog::onApplyAndClose);
	bottomRow->addWidget(btnSave_);

	mainLayout->addLayout(bottomRow);
}

void WebSourceCodeDialog::loadSourceCode()
{
	if (!source_) return;

	// 1. Resolve internal workspace path
	QString htmlDir = QString::fromStdString(OBSWorkspaceManager::GetStyleFilePath("html"));
	// Source names can contain path separators or colons in workspace tags.
	internalHtmlPath_ = htmlDir + "/" + QString::fromUtf8(obs_source_get_uuid(source_)) + ".html";

	// 2. Read dimensions and local file from source settings
	OBSDataAutoRelease settings = obs_source_get_settings(source_);
	int w = (int)obs_data_get_int(settings, "width");
	int h = (int)obs_data_get_int(settings, "height");
	if (w <= 0) w = 1920;
	if (h <= 0) h = 1080;
	// Loading dimensions must never write an empty page back to the source.
	const QSignalBlocker blockWidth(spinWidth_);
	const QSignalBlocker blockHeight(spinHeight_);
	spinWidth_->setValue(w);
	spinHeight_->setValue(h);

	QString localFile = QString::fromUtf8(obs_data_get_string(settings, "local_file"));
	const bool isLocalPage = obs_data_get_bool(settings, "is_local_file") &&
		!obs_data_get_bool(settings, "is_raw_html");
	if (isLocalPage && !localFile.isEmpty() && QFile::exists(localFile)) {
		internalHtmlPath_ = localFile;
	}

	// Read the page actually used by this source before its cached embedded copy.
	QString code;
	if (isLocalPage && QFile::exists(internalHtmlPath_)) {
		QFile f(internalHtmlPath_);
		if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			code = QString::fromUtf8(f.readAll());
			f.close();
		}
	}
	if (code.trimmed().isEmpty() && obs_data_get_bool(settings, "is_raw_html"))
		code = QString::fromUtf8(obs_data_get_string(settings, "raw_html"));
	if (code.trimmed().isEmpty())
		code = QString::fromUtf8(obs_data_get_string(settings, "embedded_code"));

	// 7. Default starter template
	if (code.trimmed().isEmpty()) {
		code = "<!DOCTYPE html>\n<html lang=\"ar\" dir=\"rtl\">\n<head>\n"
		       "  <meta charset=\"UTF-8\">\n"
		       "  <style>\n"
		       "    body {\n"
		       "      background: transparent;\n"
		       "      color: #ffffff;\n"
		       "      font-family: Arial, sans-serif;\n"
		       "      margin: 0;\n"
		       "      padding: 20px;\n"
		       "      display: flex;\n"
		       "      justify-content: center;\n"
		       "      align-items: center;\n"
		       "      height: 100vh;\n"
		       "    }\n"
		       "  </style>\n</head>\n<body>\n"
		       "  <div style='background: rgba(0,0,0,0.6); padding: 20px; border-radius: 12px; border: 2px solid #4299e1;'>\n"
		       "    <h1 style='margin:0;'>محتوى تفاعلي مباشر</h1>\n"
		       "  </div>\n"
		       "</body>\n</html>";
	}

	codeEditor_->setPlainText(code);
	isModified_ = false;
	lblStatus_->setText("تم تحميل الكود بنجاح");
}

void WebSourceCodeDialog::onCodeChanged()
{
	isModified_ = true;
	lblStatus_->setText("⚠️ تم تعديل الكود (اضغط 'تحديث المعاينة' لرؤية النتيجة)");
}

void WebSourceCodeDialog::onRefreshPreview()
{
	if (updatePreviewSource())
		lblStatus_->setText("✅ تم تحديث المعاينة الحية بنجاح");
}

bool WebSourceCodeDialog::updatePreviewSource()
{
	if (!source_ || obs_source_removed(source_) || internalHtmlPath_.isEmpty()) return false;

	QString currentCode = codeEditor_->toPlainText();
	const QString absoluteHtmlPath = QFileInfo(internalHtmlPath_).absoluteFilePath();
	QDir().mkpath(QFileInfo(absoluteHtmlPath).absolutePath());

	// 1. Save code to internal workspace file
	QSaveFile f(absoluteHtmlPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
		lblStatus_->setText(QStringLiteral("تعذر حفظ صفحة HTML: %1").arg(f.errorString()));
		return false;
	}
	const QByteArray htmlBytes = currentCode.toUtf8();
	if (f.write(htmlBytes) != htmlBytes.size() || !f.commit()) {
		lblStatus_->setText(QStringLiteral("تعذر كتابة صفحة HTML كاملة: %1").arg(f.errorString()));
		return false;
	}

	// 2. Update source settings
	OBSDataAutoRelease settings = obs_source_get_settings(source_);
	obs_data_set_bool(settings, "is_local_file", true);
	// The editor owns this page as a local workspace file. A stale raw-HTML mode
	// would make obs-browser ignore local_file and keep rendering embedded data.
	obs_data_set_bool(settings, "is_raw_html", false);
	obs_data_set_string(settings, "local_file", absoluteHtmlPath.toUtf8().constData());
	obs_data_set_string(settings, "embedded_code", currentCode.toUtf8().constData());
	obs_data_set_string(settings, "raw_html", currentCode.toUtf8().constData());
	obs_data_set_int(settings, "width", spinWidth_->value());
	obs_data_set_int(settings, "height", spinHeight_->value());
	obs_source_update(source_, settings);

	// 4. Trigger browser refresh proc if available
	proc_handler_t *ph = obs_source_get_proc_handler(source_);
	if (ph) {
		proc_handler_call(ph, "refresh", nullptr);
	}
	return true;
}

void WebSourceCodeDialog::onApplyAndClose()
{
	if (updatePreviewSource()) {
		if (main_)
			main_->SaveProject();
		accept();
	}
}

void WebSourceCodeDialog::onPresetChanged(int index)
{
	if (index <= 0) return;

	QString templateFile;
	switch (index) {
	case 1: templateFile = "دائرة يمين.html"; break;
	case 2: templateFile = "تيكتوك.html"; break;
	case 3: templateFile = "تنبيه الوقت.html"; break;
	case 4: templateFile = "تاريخ.html"; break;
	case 5: {
		codeEditor_->setPlainText(
			"<!DOCTYPE html>\n<html lang=\"ar\" dir=\"rtl\">\n<head>\n"
			"  <meta charset=\"UTF-8\">\n"
			"  <style>\n"
			"    body { background: transparent; color: white; margin: 0; font-family: sans-serif; }\n"
			"  </style>\n</head>\n<body>\n\n</body>\n</html>"
		);
		onRefreshPreview();
		return;
	}
	default: return;
	}

	QString tplPath = QString::fromStdString(OBSWorkspaceManager::GetStyleFilePath("html")) + "/" + templateFile;
	if (!QFile::exists(tplPath)) {
		tplPath = QString("E:/obs/my/%1").arg(templateFile);
	}

	if (QFile::exists(tplPath)) {
		QFile f(tplPath);
		if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
			codeEditor_->setPlainText(QString::fromUtf8(f.readAll()));
			f.close();
			onRefreshPreview();
		}
	}
}

void WebSourceCodeDialog::onOpenAdvancedProperties()
{
	OBSSource src = source_;
	OBSBasic *m = main_;
	close();
	if (m && src) {
		QTimer::singleShot(100, m, [m, src]() {
			if (!obs_source_removed(src))
				m->CreatePropertiesWindow(src);
		});
	}
}

void WebSourceCodeDialog::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	auto *dlg = static_cast<WebSourceCodeDialog *>(data);
	if (!dlg || !dlg->source_) return;

	obs_source_t *source = dlg->source_;
	uint32_t sourceCX = std::max(obs_source_get_width(source), 1u);
	uint32_t sourceCY = std::max(obs_source_get_height(source), 1u);

	gs_viewport_push();
	gs_projection_push();

	float scaleX = (float)cx / (float)sourceCX;
	float scaleY = (float)cy / (float)sourceCY;
	float scale = std::min(scaleX, scaleY);

	int drawW = (int)(sourceCX * scale);
	int drawH = (int)(sourceCY * scale);
	int drawX = (cx - drawW) / 2;
	int drawY = (cy - drawH) / 2;

	gs_set_viewport(drawX, drawY, drawW, drawH);
	gs_ortho(0.0f, (float)sourceCX, 0.0f, (float)sourceCY, -100.0f, 100.0f);

	obs_source_video_render(source);

	gs_projection_pop();
	gs_viewport_pop();
}

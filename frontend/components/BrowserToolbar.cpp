#include "BrowserToolbar.hpp"
#include "ui_browser-source-toolbar.h"
#include "widgets/OBSBasic.hpp"
#include <QPushButton>
#include "moc_BrowserToolbar.cpp"

BrowserToolbar::BrowserToolbar(QWidget *parent, OBSSource source)
	: SourceToolbar(parent, source),
	  ui(new Ui_BrowserSourceToolbar)
{
	ui->setupUi(this);

	auto *btnCode = new QPushButton(QString::fromUtf8("📝 تعديل كود HTML"), this);
	btnCode->setCursor(Qt::PointingHandCursor);
	btnCode->setStyleSheet(
		"background-color: #2b6cb0; color: white; padding: 2px 8px; border-radius: 3px; font-size: 11px;");
	connect(btnCode, &QPushButton::clicked, this, [this]() {
		OBSBasic *main = OBSBasic::Get();
		OBSSource source = GetSource();
		if (main && source) {
			main->OpenWebSourceCodeDialog(source.Get());
		}
	});
	layout()->addWidget(btnCode);
}

BrowserToolbar::~BrowserToolbar() {}

void BrowserToolbar::on_refresh_clicked()
{
	OBSSource source = GetSource();
	if (!source) {
		return;
	}

	obs_property_t *p = obs_properties_get(props, "refreshnocache");
	obs_property_button_clicked(p, source.Get());
}

#include "RecycleBinDialog.hpp"
#include "utility/RecycleBin.hpp"
#include <QMessageBox>
#include <QDateTime>
#include <QRadioButton>
#include <QFrame>

#include "moc_RecycleBinDialog.cpp"

RecycleBinDialog::RecycleBinDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle("♻️ سلة المحذوفات — استعادة المشاهد والعناصر المحذوفة");
	setLayoutDirection(Qt::RightToLeft);
	setMinimumSize(560, 480);
	resize(620, 540);

	BuildUI();
	ApplyStyles();
	RefreshList();

	connect(&RecycleBin::Instance(), &RecycleBin::ItemsChanged, this, &RecycleBinDialog::RefreshList);
}

void RecycleBinDialog::BuildUI()
{
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(14, 14, 14, 14);
	mainLayout->setSpacing(10);

	// Header banner
	auto *headerBox = new QFrame(this);
	headerBox->setObjectName("headerBox");
	auto *hbLayout = new QVBoxLayout(headerBox);
	hbLayout->setContentsMargins(12, 10, 12, 10);
	hbLayout->setSpacing(4);

	auto *lblTitle = new QLabel("♻️ سلة المحذوفات الذكية (متاحة لمدة 24 ساعة)", headerBox);
	lblTitle->setStyleSheet("font-size: 14px; font-weight: bold; color: #63b3ed;");
	hbLayout->addWidget(lblTitle);

	auto *lblDesc = new QLabel(
		"يتم الاحتفاظ بجميع المشاهد والعناصر المحذوفة تلقائياً لمدة 24 ساعة من وقت حذفها.\n"
		"يمكنك استعادة أي مشهد أو مصدر إلى موضعه الأصلي فوراً بنقرة واحدة.", headerBox);
	lblDesc->setStyleSheet("font-size: 11px; color: #a0aec0; line-height: 1.4;");
	lblDesc->setWordWrap(true);
	hbLayout->addWidget(lblDesc);

	mainLayout->addWidget(headerBox);

	// Filter Row & Items Count
	auto *filterRow = new QHBoxLayout();
	filterRow->setSpacing(8);

	filterGroup_ = new QButtonGroup(this);
	filterGroup_->setExclusive(true);

	auto makeFilterBtn = [&](int id, const QString &text, bool checked) {
		auto *btn = new QPushButton(text, this);
		btn->setCheckable(true);
		btn->setChecked(checked);
		btn->setProperty("class", "filterBtn");
		btn->setCursor(Qt::PointingHandCursor);
		filterGroup_->addButton(btn, id);
		filterRow->addWidget(btn);
	};

	makeFilterBtn(0, "📋 الكل", true);
	makeFilterBtn(1, "🎬 المشاهد المحذوفة", false);
	makeFilterBtn(2, "🧩 العناصر والمصادر", false);

	connect(filterGroup_, QOverload<int>::of(&QButtonGroup::idClicked),
		this, &RecycleBinDialog::OnFilterChanged);

	filterRow->addStretch();

	lblCount_ = new QLabel("", this);
	lblCount_->setStyleSheet("font-size: 11px; font-weight: bold; color: #cbd5e0; background: #242c3d; border-radius: 4px; padding: 3px 8px;");
	filterRow->addWidget(lblCount_);

	mainLayout->addLayout(filterRow);

	// List
	listWidget_ = new QListWidget(this);
	listWidget_->setObjectName("trashList");
	listWidget_->setLayoutDirection(Qt::RightToLeft);
	listWidget_->setSelectionMode(QAbstractItemView::NoSelection);
	mainLayout->addWidget(listWidget_, 1);

	// Bottom action buttons
	auto *bottomLayout = new QHBoxLayout();
	bottomLayout->setSpacing(8);

	auto *btnClear = new QPushButton("🗑️ تفريغ السلة بالكامل", this);
	btnClear->setObjectName("btnClearAll");
	btnClear->setFixedHeight(30);
	btnClear->setCursor(Qt::PointingHandCursor);
	connect(btnClear, &QPushButton::clicked, this, &RecycleBinDialog::OnClearAllClicked);
	bottomLayout->addWidget(btnClear);

	bottomLayout->addStretch();

	auto *btnClose = new QPushButton("إغلاق", this);
	btnClose->setObjectName("btnClose");
	btnClose->setFixedHeight(30);
	btnClose->setFixedWidth(90);
	btnClose->setCursor(Qt::PointingHandCursor);
	connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
	bottomLayout->addWidget(btnClose);

	mainLayout->addLayout(bottomLayout);
}

void RecycleBinDialog::ApplyStyles()
{
	setStyleSheet(R"(
		QDialog {
			background-color: #14171d;
			color: #e2e8f0;
			font-family: 'Segoe UI', 'Cairo', sans-serif;
		}
		#headerBox {
			background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1a202c, stop:1 #212836);
			border: 1px solid #2d3748;
			border-radius: 8px;
		}
		.filterBtn {
			background: #1f2633;
			color: #cbd5e0;
			border: 1px solid #3b4559;
			border-radius: 5px;
			padding: 4px 12px;
			font-size: 11px;
			font-weight: bold;
		}
		.filterBtn:hover {
			background: #2a3447;
			border-color: #63b3ed;
		}
		.filterBtn:checked {
			background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2b6cb0, stop:1 #3182ce);
			border-color: #63b3ed;
			color: #ffffff;
		}
		#trashList {
			background: #181d26;
			border: 1px solid #2d3748;
			border-radius: 8px;
			outline: none;
			padding: 4px;
		}
		#trashList::item {
			background: #1e2533;
			border: 1px solid #2b3547;
			border-radius: 6px;
			margin: 3px 2px;
		}
		#trashList::item:hover {
			border-color: #3b82f6;
			background: #232b3b;
		}
		#btnClearAll {
			background: #4a1d24;
			color: #fecdd3;
			border: 1px solid #991b1b;
			border-radius: 5px;
			font-size: 11px;
			font-weight: bold;
			padding: 0 12px;
		}
		#btnClearAll:hover {
			background: #7f1d1d;
			color: #ffffff;
		}
		#btnClose {
			background: #242c3d;
			color: #cbd5e0;
			border: 1px solid #3b4559;
			border-radius: 5px;
			font-size: 11px;
			font-weight: bold;
		}
		#btnClose:hover {
			background: #3182ce;
			color: #ffffff;
			border-color: #63b3ed;
		}
	)");
}

void RecycleBinDialog::OnFilterChanged(int id)
{
	currentFilter_ = id;
	RefreshList();
}

void RecycleBinDialog::RefreshList()
{
	if (!listWidget_) return;
	listWidget_->clear();

	auto items = RecycleBin::Instance().GetItems();
	int64_t now = QDateTime::currentSecsSinceEpoch();

	int shownCount = 0;
	for (const auto &it : items) {
		if (currentFilter_ == 1 && it.type != "scene") continue;
		if (currentFilter_ == 2 && it.type != "source") continue;

		shownCount++;

		auto *item = new QListWidgetItem(listWidget_);

		auto *card = new QWidget();
		card->setLayoutDirection(Qt::RightToLeft);
		auto *cardLayout = new QHBoxLayout(card);
		cardLayout->setContentsMargins(10, 8, 10, 8);
		cardLayout->setSpacing(10);

		// Icon
		auto *lblIcon = new QLabel(it.type == "scene" ? "🎬" : "🧩", card);
		lblIcon->setStyleSheet("font-size: 20px;");
		cardLayout->addWidget(lblIcon);

		// Name and details
		auto *infoLayout = new QVBoxLayout();
		infoLayout->setSpacing(2);

		auto *lblName = new QLabel(QString::fromStdString(it.name), card);
		lblName->setStyleSheet("font-weight: bold; font-size: 13px; color: #f1f5f9;");
		infoLayout->addWidget(lblName);

		// Time calculations
		int64_t elapsedSec = now - it.deletedAt;
		int elapsedMins = static_cast<int>(elapsedSec / 60);
		QString elapsedStr = elapsedMins < 60
			? QString("منذ %1 دقيقة").arg(elapsedMins)
			: QString("منذ %1 ساعة و %2 دقيقة").arg(elapsedMins / 60).arg(elapsedMins % 60);

		int64_t remainSec = (24 * 3600) - elapsedSec;
		if (remainSec < 0) remainSec = 0;
		int remainHours = static_cast<int>(remainSec / 3600);
		int remainMins  = static_cast<int>((remainSec % 3600) / 60);

		QString detailsStr = QString("النوع: %1 | تم الحذف: %2 | متبقي للاستعادة: %3 س و %4 د")
			.arg(it.type == "scene" ? "مشهد كامل" : QString("عنصر في مشهد '%1'").arg(QString::fromStdString(it.parentScene)))
			.arg(elapsedStr)
			.arg(remainHours)
			.arg(remainMins);

		auto *lblDetails = new QLabel(detailsStr, card);
		lblDetails->setStyleSheet("font-size: 11px; color: #94a3b8;");
		infoLayout->addWidget(lblDetails);

		cardLayout->addLayout(infoLayout, 1);

		// Action buttons
		QString itemId = QString::fromStdString(it.id);

		auto *btnRestore = new QPushButton("♻️ استعادة", card);
		btnRestore->setFixedHeight(26);
		btnRestore->setCursor(Qt::PointingHandCursor);
		btnRestore->setStyleSheet(R"(
			QPushButton {
				background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #15803d, stop:1 #166534);
				color: #ffffff;
				border: 1px solid #22c55e;
				border-radius: 4px;
				font-size: 11px;
				font-weight: bold;
				padding: 0 10px;
			}
			QPushButton:hover {
				background: #22c55e;
			}
		)");
		connect(btnRestore, &QPushButton::clicked, this, [this, itemId]() {
			OnRestoreClicked(itemId);
		});
		cardLayout->addWidget(btnRestore);

		auto *btnDelete = new QPushButton("✕ حذف", card);
		btnDelete->setFixedHeight(26);
		btnDelete->setCursor(Qt::PointingHandCursor);
		btnDelete->setStyleSheet(R"(
			QPushButton {
				background: #242c3d;
				color: #f87171;
				border: 1px solid #3b4559;
				border-radius: 4px;
				font-size: 11px;
				padding: 0 8px;
			}
			QPushButton:hover {
				background: #991b1b;
				color: #ffffff;
				border-color: #ef4444;
			}
		)");
		connect(btnDelete, &QPushButton::clicked, this, [this, itemId]() {
			OnDeleteClicked(itemId);
		});
		cardLayout->addWidget(btnDelete);

		item->setSizeHint(card->sizeHint());
		listWidget_->setItemWidget(item, card);
	}

	lblCount_->setText(QString("عدد المحفوظات: %1").arg(shownCount));

	if (shownCount == 0) {
		auto *emptyItem = new QListWidgetItem(listWidget_);
		auto *lblEmpty = new QLabel("سلة المحذوفات فارغة حالياً.", listWidget_);
		lblEmpty->setAlignment(Qt::AlignCenter);
		lblEmpty->setStyleSheet("color: #64748b; font-size: 13px; padding: 40px;");
		emptyItem->setSizeHint(lblEmpty->sizeHint());
		listWidget_->setItemWidget(emptyItem, lblEmpty);
	}
}

void RecycleBinDialog::OnRestoreClicked(const QString &id)
{
	QString err;
	if (RecycleBin::Instance().RestoreItem(id.toStdString(), err)) {
		QMessageBox::information(this, "تمت الاستعادة", "تمت استعادة العنصر بنجاح إلى OBS وبيئة العمل!");
		RefreshList();
	} else {
		QMessageBox::warning(this, "تنبيه", err.isEmpty() ? "تعذرت الاستعادة." : err);
	}
}

void RecycleBinDialog::OnDeleteClicked(const QString &id)
{
	if (QMessageBox::question(this, "تأكيد الحذف النهائي", "هل أنت متأكد من رغبتك في حذف هذا العنصر نهائياً من سلة المحذوفات؟",
	                          QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
		RecycleBin::Instance().PermanentlyDeleteItem(id.toStdString());
		RefreshList();
	}
}

void RecycleBinDialog::OnClearAllClicked()
{
	if (RecycleBin::Instance().GetCount() == 0) return;

	if (QMessageBox::question(this, "تأكيد تفريغ السلة", "هل أنت متأكد من تفريغ سلة المحذوفات بالكامل؟ لا يمكن التراجع عن هذه الخطوة.",
	                          QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
		RecycleBin::Instance().ClearAll();
		RefreshList();
	}
}

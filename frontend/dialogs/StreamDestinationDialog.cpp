#include "StreamDestinationDialog.hpp"
#include "RecordingResolutionDialog.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QComboBox>
#include <QFrame>
#include <QPoint>

StreamDestinationDialog::StreamDestinationDialog(const QList<MultiStreamTarget> &targets, uint32_t currentCX,
						 uint32_t currentCY, QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle(QString::fromUtf8("📡 بدء البث المباشر - تحديد وجهات ومقاسات البث"));
	setLayoutDirection(Qt::RightToLeft);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(620);
	setObjectName("StreamDestinationDialog");
	setProperty("class", "hkDialog");

	SetupUI(targets, currentCX, currentCY);
}

void StreamDestinationDialog::SetupUI(const QList<MultiStreamTarget> &targets, uint32_t currentCX, uint32_t currentCY)
{
	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(18, 16, 18, 16);
	mainLayout->setSpacing(10);

	// Header
	QLabel *titleLabel = new QLabel(QString::fromUtf8("📡 إعداد جلسة البث المباشر"), this);
	titleLabel->setObjectName("streamDestinationHeading");
	mainLayout->addWidget(titleLabel);

	QLabel *descLabel = new QLabel(
		QString::fromUtf8("حدد المنصات التي ترغب بالبث المباشر إليها بالتزامن في هذه الجلسة:"), this);
	descLabel->setWordWrap(true);
	descLabel->setObjectName("streamDestinationHint");
	mainLayout->addWidget(descLabel);

	QLabel *lblResolution = new QLabel(QString::fromUtf8("اختر المنصات ومقاس البث لكل منصة:"), this);
	lblResolution->setObjectName("streamDestinationResolutionLabel");
	mainLayout->addWidget(lblResolution);

	// Quick Select / Deselect row
	QHBoxLayout *quickLayout = new QHBoxLayout();
	QPushButton *btnSelectAll = new QPushButton(QString::fromUtf8("☑️ تحديد الكل"), this);
	btnSelectAll->setProperty("class", "quickBtn");
	connect(btnSelectAll, &QPushButton::clicked, this, &StreamDestinationDialog::OnSelectAllClicked);

	QPushButton *btnDeselectAll = new QPushButton(QString::fromUtf8("◻️ إلغاء التحديد"), this);
	btnDeselectAll->setProperty("class", "quickBtn");
	connect(btnDeselectAll, &QPushButton::clicked, this, &StreamDestinationDialog::OnDeselectAllClicked);

	quickLayout->addWidget(btnSelectAll);
	quickLayout->addWidget(btnDeselectAll);
	quickLayout->addStretch();
	mainLayout->addLayout(quickLayout);

	// Each saved destination has its own session checkbox and stored resolution.
	QVBoxLayout *cardsLayout = new QVBoxLayout();
	cardsLayout->setSpacing(6);
	bool defaultSelectionAssigned = false;

	for (const auto &t : targets) {
		targetOrder_.append(t.id);
		QFrame *card = new QFrame(this);
		card->setProperty("class", "cardFrame");
		card->setObjectName(QString("streamDestinationRow_%1").arg(t.id));
		card->setLayoutDirection(Qt::LeftToRight);
		auto *row = new QHBoxLayout(card);
		row->setContentsMargins(10, 6, 10, 6);
		row->setSpacing(12);

		auto *resolution = new QComboBox(card);
		resolution->setLayoutDirection(Qt::RightToLeft);
		resolution->setIconSize(QSize(22, 22));
		resolution->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		resolution->setMinimumWidth(250);
		resolution->setAccessibleName(QString::fromUtf8("مقاس البث لـ%1").arg(t.name));
		const QString currentCanvasLabel = QString::fromUtf8("مطابقة مقاس المشهد الحالي (%1 × %2)")
						  .arg(currentCX).arg(currentCY);
		resolution->addItem(currentCanvasLabel, QPoint(0, 0));
		resolution->addItem(RecordingResolutionDialog::MakeAspectRatioIcon(16, 9, 22),
				    QString::fromUtf8("1920 × 1080 — عريض"), QPoint(1920, 1080));
		resolution->addItem(RecordingResolutionDialog::MakeAspectRatioIcon(9, 16, 22),
				    QString::fromUtf8("1080 × 1920 — طولي"), QPoint(1080, 1920));
		resolution->addItem(RecordingResolutionDialog::MakeAspectRatioIcon(1, 1, 22),
				    QString::fromUtf8("1080 × 1080 — مربع"), QPoint(1080, 1080));
		if (t.cx > 0 && t.cy > 0 && !((t.cx == 1920 && t.cy == 1080) ||
						 (t.cx == 1080 && t.cy == 1920) ||
						 (t.cx == 1080 && t.cy == 1080))) {
			resolution->addItem(QString::fromUtf8("%1 × %2 — مخصص").arg(t.cx).arg(t.cy),
				     QPoint(static_cast<int>(t.cx), static_cast<int>(t.cy)));
		}
		const QPoint savedResolution(static_cast<int>(t.cx), static_cast<int>(t.cy));
		int savedIndex = resolution->findData(savedResolution);
		if (savedIndex < 0)
			savedIndex = resolution->findData(QPoint(0, 0));
		resolution->setCurrentIndex(savedIndex);
		resolutionCombos_[t.id] = resolution;

		QCheckBox *cb = new QCheckBox(QString("%1 %2").arg(t.icon, t.name), card);
		cb->setLayoutDirection(Qt::RightToLeft);
		const bool selectedByDefault = t.enabled && !defaultSelectionAssigned;
		cb->setChecked(selectedByDefault);
		defaultSelectionAssigned = defaultSelectionAssigned || selectedByDefault;
		connect(cb, &QCheckBox::toggled, this, &StreamDestinationDialog::OnCheckboxToggled);
		checkBoxes_[t.id] = cb;
		row->addWidget(resolution, 1);
		row->addStretch(1);
		row->addWidget(cb, 0, Qt::AlignRight | Qt::AlignVCenter);

		cardsLayout->addWidget(card);
	}

	mainLayout->addLayout(cardsLayout);

	// Separator
	QFrame *line = new QFrame(this);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Sunken);
	line->setObjectName("streamDestinationSeparator");
	mainLayout->addWidget(line);

	// Bottom action buttons
	QHBoxLayout *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(10);

	btnStart_ = new QPushButton(QString::fromUtf8("▶️ بدء البث"), this);
	btnStart_->setObjectName("btnStart");
	btnStart_->setCursor(Qt::PointingHandCursor);
	connect(btnStart_, &QPushButton::clicked, this, &StreamDestinationDialog::accept);

	btnCancel_ = new QPushButton(QString::fromUtf8("إلغاء"), this);
	btnCancel_->setObjectName("btnCancel");
	btnCancel_->setCursor(Qt::PointingHandCursor);
	connect(btnCancel_, &QPushButton::clicked, this, &QDialog::reject);

	btnLayout->addWidget(btnStart_);
	btnLayout->addWidget(btnCancel_);
	btnLayout->addStretch();

	mainLayout->addLayout(btnLayout);

	OnCheckboxToggled();
}

void StreamDestinationDialog::accept()
{
	QDialog::accept();
}

void StreamDestinationDialog::OnSelectAllClicked()
{
	for (auto cb : checkBoxes_) {
		cb->setChecked(true);
	}
}

void StreamDestinationDialog::OnDeselectAllClicked()
{
	for (auto cb : checkBoxes_) {
		cb->setChecked(false);
	}
}

void StreamDestinationDialog::OnCheckboxToggled()
{
	bool anyChecked = false;
	for (auto cb : checkBoxes_) {
		if (cb->isChecked()) {
			anyChecked = true;
			break;
		}
	}
	if (btnStart_) {
		btnStart_->setEnabled(anyChecked);
	}
}

QStringList StreamDestinationDialog::GetSelectedTargetIds() const
{
	QStringList selected;
	for (const QString &id : targetOrder_) {
		if (checkBoxes_.value(id) && checkBoxes_.value(id)->isChecked()) {
			selected.append(id);
		}
	}
	return selected;
}

QMap<QString, QSize> StreamDestinationDialog::GetSelectedResolutions() const
{
	QMap<QString, QSize> selected;
	for (const QString &id : targetOrder_) {
		QComboBox *combo = resolutionCombos_.value(id);
		if (!combo)
			continue;
		const QPoint size = combo->currentData().toPoint();
		selected.insert(id, QSize(size.x(), size.y()));
	}
	return selected;
}

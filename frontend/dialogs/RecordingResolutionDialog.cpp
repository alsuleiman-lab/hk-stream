#include "RecordingResolutionDialog.hpp"
#include "OBSApp.hpp"
#include <utility/platform.hpp>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QApplication>
#include <QIconEngine>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QPoint>
#include <cmath>
#include <limits>

namespace {
class AspectRatioIcon final : public QIconEngine {
public:
	AspectRatioIcon(int w, int h) : w_(w), h_(h) {}
	QIconEngine *clone() const override { return new AspectRatioIcon(w_, h_); }
	void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
	{
		painter->save();
		painter->setRenderHint(QPainter::Antialiasing);
		const auto palette = QApplication::palette();
		const bool systemTheme = HighContrastEnabled() || (App()->GetTheme() && App()->GetTheme()->id.endsWith(".System"));
		const QColor ink = palette.color(mode == QIcon::Selected && !systemTheme ? QPalette::HighlightedText : QPalette::Text);
		painter->setPen(QPen(ink, qMax(1.5, rect.width() / 18.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter->setBrush(Qt::NoBrush);
		const qreal size = qMin(rect.width(), rect.height());
		const qreal aspect = qreal(w_) / qMax(1, h_);
		QSizeF screen(size - 6, (size - 6) / aspect);
		if (screen.height() > size - 6) screen = QSizeF((size - 6) * aspect, size - 6);
		QRectF frame(QPointF(rect.center().x() - screen.width() / 2, rect.center().y() - screen.height() / 2), screen);
		if (aspect > 1.5) frame.translate(0, -size * .08);
		painter->drawRoundedRect(frame, 2.5, 2.5);
		if (aspect > 1.5) {
			const qreal x = frame.center().x();
			painter->drawLine(QPointF(x, frame.bottom()), QPointF(x, frame.bottom() + size * .16));
			painter->drawLine(QPointF(x - size * .18, frame.bottom() + size * .16), QPointF(x + size * .18, frame.bottom() + size * .16));
		} else if (aspect < .65) {
			painter->drawLine(QPointF(frame.center().x() - 2, frame.bottom() - 3), QPointF(frame.center().x() + 2, frame.bottom() - 3));
		}
		painter->restore();
	}
	QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
	{
		QPixmap image(size); image.fill(Qt::transparent);
		QPainter painter(&image); paint(&painter, QRect(QPoint(), size), mode, state); return image;
	}
private:
	int w_, h_;
};

class ResolutionChoiceDelegate final : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		if (HighContrastEnabled() || (App()->GetTheme() && App()->GetTheme()->id.endsWith(".System"))) {
			QStyledItemDelegate::paint(painter, option, index);
			return;
		}
		QStyleOptionViewItem opt(option); initStyleOption(&opt, index);
		const QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
		style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
		if (!(opt.features & QStyleOptionViewItem::HasCheckIndicator)) return;
		QRectF indicator = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, opt.widget);
		indicator.adjust(1, 1, -1, -1);
		const bool selected = opt.state & QStyle::State_Selected;
		const bool checked = opt.checkState == Qt::Checked;
		const QColor ink = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
		const QColor fill = opt.palette.color(checked ? QPalette::Highlight : (selected ? QPalette::Highlight : QPalette::Base));
		painter->save(); painter->setRenderHint(QPainter::Antialiasing);
		painter->setPen(QPen(ink, 1.8)); painter->setBrush(fill);
		painter->drawRoundedRect(indicator, 3, 3);
		if (checked) {
			painter->setPen(QPen(opt.palette.color(QPalette::HighlightedText), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			const QPointF points[] = {{indicator.left() + indicator.width() * .22, indicator.center().y()},
				{indicator.left() + indicator.width() * .43, indicator.top() + indicator.height() * .72},
				{indicator.left() + indicator.width() * .79, indicator.top() + indicator.height() * .28}};
			painter->drawPolyline(points, 3);
		}
		painter->restore();
	}
};
}

QIcon RecordingResolutionDialog::MakeAspectRatioIcon(int wRatio, int hRatio, int iconSize)
{
	Q_UNUSED(iconSize);
	return QIcon(new AspectRatioIcon(wRatio, hRatio));
}

RecordingResolutionDialog::RecordingResolutionDialog(uint32_t currentCX, uint32_t currentCY, QWidget *parent)
	: QDialog(parent), currentCX_(currentCX), currentCY_(currentCY), selectedCX_(currentCX), selectedCY_(currentCY)
{
	setObjectName("RecordingResolutionDialog");
	setWindowTitle("🎬 خيارات تسجيل الفيديو - HK Stream");
	setLayoutDirection(Qt::RightToLeft);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(500, 440);

	BuildUI();
	ApplyStyles();
	UpdateConfirmButton();
}

void RecordingResolutionDialog::BuildUI()
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(20, 20, 20, 20);
	layout->setSpacing(12);

	auto *lblTitle = new QLabel("🎬 اختر مقاسات التسجيل", this);
	lblTitle->setObjectName("recordingResolutionHeading");
	layout->addWidget(lblTitle);

	auto *lblSubtitle = new QLabel(
		"حدد مقاساً واحداً أو مقاسين للتسجيل المتزامن في ملفين منفصلين (مثل يوتيوب عريض + تيكتوك طولي):",
		this);
	lblSubtitle->setObjectName("recordingResolutionHint");
	lblSubtitle->setWordWrap(true);
	layout->addWidget(lblSubtitle);

	listWidget_ = new QListWidget(this);
	listWidget_->setObjectName("recordingResolutionList");
	listWidget_->setItemDelegate(new ResolutionChoiceDelegate(listWidget_));
	listWidget_->setIconSize(QSize(36, 36));
	listWidget_->setSpacing(4);

	struct ResOption {
		QString title;
		int wRatio;
		int hRatio;
		uint32_t cx;
		uint32_t cy;
	};

	QList<ResOption> options = {
		{"Full HD عريض (1920 × 1080)", 16, 9, 1920, 1080},
		{"Full HD طولي (1080 × 1920)", 9, 16, 1080, 1920},
		{"مربع (1080 × 1080)", 1, 1, 1080, 1080},
	};

	const char *savedResStr = config_get_string(App()->GetUserConfig(), "BasicWindow", "LastSelectedRecordingResolutions");
	QStringList savedTokens;
	if (savedResStr && *savedResStr) {
		savedTokens = QString::fromUtf8(savedResStr).split(";", Qt::SkipEmptyParts);
	}

	updatingChecks_ = true;
	int defaultIndex = 0;
	int idx = 0;
	const double currentAspect = currentCY_ ? static_cast<double>(currentCX_) / currentCY_ : 16.0 / 9.0;
	double bestAspectDistance = std::numeric_limits<double>::max();
	for (const auto &opt : options) {
		auto *item = new QListWidgetItem(listWidget_);
		item->setIcon(MakeAspectRatioIcon(opt.wRatio, opt.hRatio, 36));
		item->setText(opt.title);
		item->setData(Qt::UserRole, QPoint(opt.cx, opt.cy));
		item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);

		bool shouldCheck = false;
		if (!savedTokens.isEmpty()) {
			QString token = QString("%1x%2").arg(opt.cx).arg(opt.cy);
			shouldCheck = savedTokens.contains(token);
		} else {
			shouldCheck = (opt.cx == currentCX_ && opt.cy == currentCY_);
		}

		if (shouldCheck) {
			item->setCheckState(Qt::Checked);
			defaultIndex = idx;
		} else {
			item->setCheckState(Qt::Unchecked);
		}
		const double distance = std::abs(currentAspect - static_cast<double>(opt.wRatio) / opt.hRatio);
		if (savedTokens.isEmpty() && distance < bestAspectDistance) {
			bestAspectDistance = distance;
			defaultIndex = idx;
		}
		idx++;
	}
	bool anyChecked = false;
	for (int i = 0; i < listWidget_->count(); ++i)
		anyChecked = anyChecked || listWidget_->item(i)->checkState() == Qt::Checked;
	if (!anyChecked) {
		double nearestDistance = std::numeric_limits<double>::max();
		for (int i = 0; i < options.size(); ++i) {
			const auto &opt = options[i];
			const double distance = std::abs(currentAspect - static_cast<double>(opt.wRatio) / opt.hRatio);
			if (distance < nearestDistance) {
				nearestDistance = distance;
				defaultIndex = i;
			}
		}
		listWidget_->item(defaultIndex)->setCheckState(Qt::Checked);
	}
	updatingChecks_ = false;

	listWidget_->setCurrentRow(defaultIndex);
	connect(listWidget_, &QListWidget::itemDoubleClicked, this, &RecordingResolutionDialog::OnConfirm);
	connect(listWidget_, &QListWidget::itemClicked, this, &RecordingResolutionDialog::OnItemClicked);
	connect(listWidget_, &QListWidget::itemChanged, this, &RecordingResolutionDialog::OnItemChanged);

	layout->addWidget(listWidget_, 1);

	auto *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(10);
	btnLayout->addStretch();

	auto *btnCancel = new QPushButton("إلغاء", this);
	btnCancel->setObjectName("btnCancel");
	btnCancel->setCursor(Qt::PointingHandCursor);
	connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
	btnLayout->addWidget(btnCancel);

	btnConfirm_ = new QPushButton("🔴 بدء التسجيل الآن", this);
	btnConfirm_->setObjectName("btnConfirm");
	btnConfirm_->setCursor(Qt::PointingHandCursor);
	btnConfirm_->setDefault(true);
	connect(btnConfirm_, &QPushButton::clicked, this, &RecordingResolutionDialog::OnConfirm);
	btnLayout->addWidget(btnConfirm_);

	layout->addLayout(btnLayout);
}

void RecordingResolutionDialog::OnItemClicked(QListWidgetItem *item)
{
	if (!item || updatingChecks_) return;
	UpdateConfirmButton();
}

void RecordingResolutionDialog::OnItemChanged(QListWidgetItem *item)
{
	if (updatingChecks_) return;
	if (item && item->checkState() == Qt::Checked) {
		int checkedCount = 0;
		for (int i = 0; i < listWidget_->count(); ++i) {
			if (listWidget_->item(i)->checkState() == Qt::Checked) {
				++checkedCount;
			}
		}
		if (checkedCount > 2) {
			updatingChecks_ = true;
			item->setCheckState(Qt::Unchecked);
			updatingChecks_ = false;
			QMessageBox::information(this, "مقاسان كحد أقصى",
					 "يمكن تسجيل مقاسين معاً في الوقت الحالي. ألغِ تحديد أحد المقاسات لاختيار هذا المقاس.");
		}
	}
	UpdateConfirmButton();
}

void RecordingResolutionDialog::UpdateConfirmButton()
{
	if (!btnConfirm_ || !listWidget_) return;

	int checkedCount = 0;
	for (int i = 0; i < listWidget_->count(); ++i) {
		if (listWidget_->item(i)->checkState() == Qt::Checked) {
			checkedCount++;
		}
	}

	if (checkedCount == 0) {
		btnConfirm_->setText("🔴 اختر مقاساً للبدء");
		btnConfirm_->setEnabled(false);
	} else if (checkedCount == 1) {
		btnConfirm_->setText("🔴 بدء التسجيل الآن");
		btnConfirm_->setEnabled(true);
	} else {
		btnConfirm_->setText(QString("🔴 بدء التسجيل المتزامن (%1 مقاسات معاً)").arg(checkedCount));
		btnConfirm_->setEnabled(true);
	}
}

void RecordingResolutionDialog::ApplyStyles()
{
	// Application theme variables are resolved by HK's theme loader, not by Qt
	// widget-local stylesheets. Keep the dialog in that shared cascade.
	setStyleSheet(QString());
}

void RecordingResolutionDialog::OnConfirm()
{
	selectedResolutions_.clear();

	for (int i = 0; i < listWidget_->count(); ++i) {
		auto *item = listWidget_->item(i);
		if (item && item->checkState() == Qt::Checked) {
			QPoint pt = item->data(Qt::UserRole).toPoint();
			ResolutionChoice choice;
			choice.cx = static_cast<uint32_t>(pt.x());
			choice.cy = static_cast<uint32_t>(pt.y());
			choice.title = item->text();
			selectedResolutions_.push_back(choice);
		}
	}

	// Fallback: if user didn't check any, use current highlighted item
	if (selectedResolutions_.empty() && listWidget_->currentItem()) {
		auto *item = listWidget_->currentItem();
		QPoint pt = item->data(Qt::UserRole).toPoint();
		ResolutionChoice choice;
		choice.cx = static_cast<uint32_t>(pt.x());
		choice.cy = static_cast<uint32_t>(pt.y());
		choice.title = item->text();
		selectedResolutions_.push_back(choice);
	}

	if (!selectedResolutions_.empty()) {
		selectedCX_ = selectedResolutions_[0].cx;
		selectedCY_ = selectedResolutions_[0].cy;

		QStringList savedList;
		for (const auto &choice : selectedResolutions_) {
			savedList << QString("%1x%2").arg(choice.cx).arg(choice.cy);
		}
		config_set_string(App()->GetUserConfig(), "BasicWindow", "LastSelectedRecordingResolutions", savedList.join(";").toUtf8().constData());
		config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	}

	accept();
}

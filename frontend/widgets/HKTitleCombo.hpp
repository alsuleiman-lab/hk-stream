#pragma once

#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPainter>
#include <QScreen>
#include <QVBoxLayout>
#include <QStyleOptionComboBox>
#include <QTextOption>

// A single selector with a session-only editor inside its popup.
class HKTitleCombo final : public QComboBox {
public:
	explicit HKTitleCombo(QWidget *parent = nullptr) : QComboBox(parent)
	{
		setLayoutDirection(Qt::RightToLeft);

		popup_ = new QFrame(this, Qt::Popup);
		popup_->setObjectName("titleQuickPopup");
		popup_->setLayoutDirection(Qt::RightToLeft);

		auto layout = new QVBoxLayout(popup_);
		layout->setContentsMargins(8, 8, 8, 8);
		layout->setSpacing(8);

		list_ = new QListWidget(popup_);
		list_->setObjectName("titleQuickList");
		list_->setLayoutDirection(Qt::RightToLeft);
		list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		layout->addWidget(list_);

		auto *separator = new QFrame(popup_);
		separator->setFrameShape(QFrame::HLine);
		separator->setFrameShadow(QFrame::Sunken);
		layout->addWidget(separator);

		auto editor = new QHBoxLayout;
		editor->setSpacing(6);
		temporaryEdit = new QLineEdit(popup_);
		temporaryEdit->setObjectName("txtTempTitle");
		temporaryEdit->setLayoutDirection(Qt::RightToLeft);
		temporaryEdit->setAlignment(Qt::AlignRight);
		temporaryEdit->setPlaceholderText(QString::fromUtf8("اكتب عنواناً مؤقتاً هنا..."));
		temporaryEdit->setAccessibleName(QString::fromUtf8("العنوان المؤقت"));
		editor->addWidget(temporaryEdit, 1);

		applyTemporary = new QPushButton(QString::fromUtf8("تطبيق"), popup_);
		applyTemporary->setObjectName("btnApplyTempTitle");
		applyTemporary->setMinimumWidth(64);
		applyTemporary->setCursor(Qt::PointingHandCursor);
		editor->addWidget(applyTemporary);
		layout->addLayout(editor);

		restorePermanent = new QPushButton(QString::fromUtf8("↺ استعادة العنوان الدائم"), popup_);
		restorePermanent->setObjectName("btnCancelTempTitle");
		restorePermanent->setCursor(Qt::PointingHandCursor);
		layout->addWidget(restorePermanent);

		connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { select(item); });
		connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { select(item); });
		connect(this, &QComboBox::currentTextChanged, this, [this](const QString &txt) { setToolTip(txt); });
	}

	void showPopup() override
	{
		list_->clear();
		for (int i = 0; i < count(); ++i) {
			auto *item = new QListWidgetItem(itemText(i), list_);
			item->setData(Qt::UserRole, itemData(i));
			item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		}
		list_->setCurrentRow(currentIndex());
		list_->ensurePolished();

		int listHeight = 4;
		for (int i = 0; i < qMin(6, count()); ++i)
			listHeight += qMax(32, list_->sizeHintForRow(i));
		list_->setFixedHeight(qMax(36, listHeight));
		popup_->setFixedWidth(qMax(360, width()));
		popup_->adjustSize();

		QPoint position = mapToGlobal(QPoint(width() - popup_->width(), height() + 4));
		if (auto screen = QGuiApplication::screenAt(position)) {
			const QRect bounds = screen->availableGeometry();
			position.setX(qBound(bounds.left(), position.x(), qMax(bounds.left(), bounds.right() - popup_->width() + 1)));
			if (position.y() + popup_->height() > bounds.bottom())
				position.setY(mapToGlobal(QPoint(0, 0)).y() - popup_->height() - 4);
		}
		popup_->move(position);
		popup_->show();
		list_->setFocus();
	}

	void hidePopup() override { popup_->hide(); }

	QLineEdit *temporaryEdit = nullptr;
	QPushButton *applyTemporary = nullptr;
	QPushButton *restorePermanent = nullptr;

protected:
	void paintEvent(QPaintEvent *event) override
	{
		Q_UNUSED(event);
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);

		// Draw base combobox styling without letting QStyle draw LTR text
		QStyleOptionComboBox opt;
		initStyleOption(&opt);
		opt.currentText = QString();
		style()->drawComplexControl(QStyle::CC_ComboBox, &opt, &painter, this);

		// RTL chevron on the far left side
		painter.setPen(QPen(palette().color(QPalette::Text), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		const qreal x = 16.0;
		const qreal y = height() / 2.0;
		const QPointF points[] = {{x - 4.0, y - 2.0}, {x, y + 2.0}, {x + 4.0, y - 2.0}};
		painter.drawPolyline(points, 3);

		// Draw text with strict RTL right alignment and dynamic auto-fit
		QString text = currentText();
		if (!text.isEmpty()) {
			painter.setPen(palette().color(QPalette::Text));
			QFont f = font();

			// Bounds: start at 28px from left (leaving room for arrow) and right margin at width() - 14px
			QRect textRect(28, 0, width() - 42, height());
			const int availWidth = textRect.width();

			// Auto-scale font down if title is long so it fits completely inside the title box
			QFontMetrics fm(f);
			qreal pointSize = f.pointSizeF();
			if (pointSize <= 0) pointSize = (f.pixelSize() > 0) ? (f.pixelSize() * 0.75) : 9.5;
			while (fm.horizontalAdvance(text) > availWidth && pointSize > 7.0) {
				pointSize -= 0.5;
				f.setPointSizeF(pointSize);
				fm = QFontMetrics(f);
			}

			// If still wider than available space even at 7pt, elide in the middle
			QString displayText = text;
			if (fm.horizontalAdvance(displayText) > availWidth) {
				displayText = fm.elidedText(text, Qt::ElideMiddle, availWidth);
			}

			painter.setFont(f);
			QTextOption textOption(Qt::AlignRight | Qt::AlignVCenter);
			textOption.setTextDirection(Qt::RightToLeft);
			textOption.setWrapMode(QTextOption::NoWrap);
			painter.drawText(textRect, displayText, textOption);
		}
	}

private:
	void select(QListWidgetItem *item)
	{
		const int index = list_->row(item);
		if (index < 0) return;
		setCurrentIndex(index);
		emit activated(index); // Selecting the same permanent title also cancels a temporary one.
		hidePopup();
		setFocus();
	}

	QFrame *popup_ = nullptr;
	QListWidget *list_ = nullptr;
};

#include "pdfgriditemdelegate.h"

#include "foldercontentmodel.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>

namespace {
constexpr int kPadding = 8;
constexpr int kIconTextSpacing = 6;
constexpr int kLineSpacing = 2;
constexpr int kNameLines = 2;
}

void PdfGridItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    painter->save();

    QStyle *itemStyle = option.widget ? option.widget->style() : QApplication::style();
    itemStyle->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, option.widget);

    const QRect rect = option.rect.adjusted(kPadding, kPadding, -kPadding, -kPadding);
    if (rect.width() <= 0 || rect.height() <= 0) {
        painter->restore();
        return;
    }

    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    const int iconSize = qMin(option.decorationSize.width(), rect.width());
    const QRect iconRect(rect.left() + (rect.width() - iconSize) / 2, rect.top(), iconSize, iconSize);
    if (!icon.isNull())
        icon.paint(painter, iconRect, Qt::AlignCenter);

    const bool selected = option.state & QStyle::State_Selected;
    const QPalette::ColorRole textRole = selected ? QPalette::HighlightedText : QPalette::Text;

    const QString name = index.data(Qt::DisplayRole).toString();
    const QFontMetrics nameMetrics(option.font);
    const QRect nameRect(rect.left(), iconRect.bottom() + kIconTextSpacing, rect.width(),
                          nameMetrics.height() * kNameLines);

    painter->setFont(option.font);
    painter->setPen(option.palette.color(textRole));
    painter->drawText(nameRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, name);

    const QString pages = index.data(FolderContentModel::PageCountRole).toString();
    if (!pages.isEmpty()) {
        QFont pagesFont = option.font;
        pagesFont.setPointSizeF(pagesFont.pointSizeF() * 0.85);
        const QFontMetrics pagesMetrics(pagesFont);
        const QRect pagesRect(rect.left(), nameRect.bottom() + kLineSpacing, rect.width(), pagesMetrics.height());
        const QString elidedPages = pagesMetrics.elidedText(pages, Qt::ElideRight, rect.width());

        painter->setFont(pagesFont);
        painter->setOpacity(0.65);
        painter->drawText(pagesRect, Qt::AlignHCenter | Qt::AlignTop, elidedPages);
    }

    painter->restore();
}

QSize PdfGridItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);

    const int iconSize = option.decorationSize.width();
    const int cellWidth = iconSize + 2 * kPadding;

    const QFontMetrics nameMetrics(option.font);
    const int nameHeight = nameMetrics.height() * kNameLines;

    QFont pagesFont = option.font;
    pagesFont.setPointSizeF(pagesFont.pointSizeF() * 0.85);
    const QFontMetrics pagesMetrics(pagesFont);

    const int height =
        kPadding + iconSize + kIconTextSpacing + nameHeight + kLineSpacing + pagesMetrics.height() + kPadding;
    return QSize(cellWidth, height);
}

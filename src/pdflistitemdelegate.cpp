#include "pdflistitemdelegate.h"

#include "foldercontentmodel.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>

namespace {
constexpr int kPadding = 6;
constexpr int kSpacing = 10;
constexpr int kLineSpacing = 2;
constexpr int kMinRowHeight = 40;
}

void PdfListItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    painter->save();

    QStyle *itemStyle = option.widget ? option.widget->style() : QApplication::style();
    itemStyle->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter, option.widget);

    const QRect rect = option.rect.adjusted(kPadding, kPadding, -kPadding, -kPadding);

    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    // Never let the icon claim more than the row's own width: in a
    // sufficiently narrow content pane rect.height() (used as the icon's
    // square size) can exceed rect.width(), which used to push textRect's
    // width negative and make QFontMetrics::elidedText() return an empty
    // string — the name would just vanish.
    const int iconSize = qMin(rect.height(), rect.width());
    const QRect iconRect(rect.left(), rect.top(), iconSize, iconSize);
    if (!icon.isNull())
        icon.paint(painter, iconRect, Qt::AlignCenter);

    const int textLeft = iconRect.right() + (icon.isNull() ? 0 : kSpacing);
    const int textWidth = qMax(0, rect.right() - textLeft);
    const QRect textRect(textLeft, rect.top(), textWidth, rect.height());

    const bool selected = option.state & QStyle::State_Selected;
    const QPalette::ColorRole textRole = selected ? QPalette::HighlightedText : QPalette::Text;

    QFont nameFont = option.font;
    nameFont.setBold(true);
    const QFontMetrics nameMetrics(nameFont);
    const QString name = index.data(Qt::DisplayRole).toString();
    if (textRect.width() > 0) {
        const QString elidedName = nameMetrics.elidedText(name, Qt::ElideMiddle, textRect.width());
        const QRect nameRect(textRect.left(), textRect.top(), textRect.width(), nameMetrics.height());

        painter->setFont(nameFont);
        painter->setPen(option.palette.color(textRole));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        const QString metadata = index.data(FolderContentModel::MetadataRole).toString();
        if (!metadata.isEmpty()) {
            QFont metaFont = option.font;
            metaFont.setPointSizeF(metaFont.pointSizeF() * 0.9);
            const QFontMetrics metaMetrics(metaFont);
            const QString elidedMeta = metaMetrics.elidedText(metadata, Qt::ElideRight, textRect.width());
            const QRect metaRect(textRect.left(), nameRect.bottom() + kLineSpacing, textRect.width(),
                                  metaMetrics.height());

            painter->setFont(metaFont);
            painter->setOpacity(0.65);
            painter->drawText(metaRect, Qt::AlignLeft | Qt::AlignVCenter, elidedMeta);
        }
    }

    painter->restore();
}

QSize PdfListItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    const int rowHeight = option.decorationSize.height() + 2 * kPadding;
    return QSize(option.rect.width(), qMax(rowHeight, kMinRowHeight));
}

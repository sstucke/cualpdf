#pragma once

#include <QStyledItemDelegate>

// Paints each cell of folderContentView in Thumbnails (icon grid) mode:
// icon on top, word-wrapped file name below it, and a dimmed page-count
// line under the name. sizeHint accounts for the wrapped name's actual
// height so cells never end up too short to show their own text.
class PdfGridItemDelegate final : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

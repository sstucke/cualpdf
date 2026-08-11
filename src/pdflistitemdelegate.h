#pragma once

#include <QStyledItemDelegate>

// Paints each row of folderContentView as an icon on the left and, to its
// right, an elided (never wrapped) file name on top of a dimmed metadata
// line (page count / created / modified). Row height follows the view's
// current icon size, so it tracks the zoom slider.
class PdfListItemDelegate final : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

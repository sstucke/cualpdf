#include "tips.h"

#include <QCoreApplication>

QStringList applicationTips()
{
    return {
        QCoreApplication::translate(
            "Tips", "Use Ctrl or Command while clicking to select multiple PDF pages."),
        QCoreApplication::translate(
            "Tips", "Use Shift while clicking to select a continuous range of PDF pages."),
        QCoreApplication::translate(
            "Tips", "Switch to region selection to draw a crop area directly on a page."),
        QCoreApplication::translate(
            "Tips", "A crop region can be applied to the current page, every page, or a page range."),
        QCoreApplication::translate(
            "Tips", "Hold Ctrl or Command and use the mouse wheel to zoom the PDF."),
        QCoreApplication::translate(
            "Tips", "Right-click a PDF or folder to reveal it in the system file explorer."),
        QCoreApplication::translate(
            "Tips", "Pin frequently used folders to Favorites from the folder tree."),
        QCoreApplication::translate(
            "Tips", "Use the Sort menu to order files by name, size, or modification date."),
        QCoreApplication::translate(
            "Tips", "Page rotation applies to every page currently selected."),
    };
}

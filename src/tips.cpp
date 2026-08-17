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
            "Tips", "Use the page selection menu to select all, none, even, or odd pages."),
        QCoreApplication::translate(
            "Tips", "Switch to region selection to draw a crop area directly on a page."),
        QCoreApplication::translate(
            "Tips", "A crop region can be applied to the current page, every page, or a page range."),
        QCoreApplication::translate(
            "Tips", "Hold Ctrl or Command and use the mouse wheel to zoom the PDF."),
        QCoreApplication::translate(
            "Tips", "Use the page number field or the arrow buttons to navigate directly through a PDF."),
        QCoreApplication::translate(
            "Tips", "Choose Continuous or Discrete view depending on how you want to browse pages."),
        QCoreApplication::translate(
            "Tips", "Fit Page, Fit Width, and Fit Two Columns adapt the PDF to the available space."),
        QCoreApplication::translate(
            "Tips", "Turn on Organize Pages, then drag one or more selected pages to an insertion marker."),
        QCoreApplication::translate(
            "Tips", "Right-click an insertion marker to add a blank page, paste pages, or insert another PDF."),
        QCoreApplication::translate(
            "Tips", "Cut or copy selected pages with Ctrl or Command plus X or C, then paste them at an insertion marker."),
        QCoreApplication::translate(
            "Tips", "Extract PDF saves the selected pages as a new PDF without changing the open document."),
        QCoreApplication::translate(
            "Tips", "Undo and Redo keep up to 20 page edits, including crop, rotation, and page organization."),
        QCoreApplication::translate(
            "Tips", "Save writes edits to the open PDF; Preferences can create and limit timestamped backups first."),
        QCoreApplication::translate(
            "Tips", "Right-click a PDF or folder to reveal it in the system file explorer."),
        QCoreApplication::translate(
            "Tips", "Right-click a PDF in the explorer to open it with the system default application."),
        QCoreApplication::translate(
            "Tips", "Pin frequently used folders to Favorites from the folder tree."),
        QCoreApplication::translate(
            "Tips", "Use the Sort menu to order files by name, size, or modification date."),
        QCoreApplication::translate(
            "Tips", "The Sort menu can keep folders before files in every sort order."),
        QCoreApplication::translate(
            "Tips", "Switch the file explorer between Thumbnails, Details, and Compact List views."),
        QCoreApplication::translate(
            "Tips", "Use the explorer zoom control to resize thumbnails; select a PDF to preview its pages and details."),
        QCoreApplication::translate(
            "Tips", "Open Folder chooses a location, while the location field lets you type a folder path directly."),
        QCoreApplication::translate(
            "Tips", "The Recent menu provides quick access to PDFs you opened before."),
        QCoreApplication::translate(
            "Tips", "Use Ctrl or Command plus W to close the current tab."),
        QCoreApplication::translate(
            "Tips", "The Window menu can close all tabs, close the other tabs, or reopen the file explorer."),
        QCoreApplication::translate(
            "Tips", "Preferences controls whether Close All preserves the file explorer tab."),
        QCoreApplication::translate(
            "Tips", "Preferences also controls startup tips, automatic tip closing, and searchable settings."),
        QCoreApplication::translate(
            "Tips", "Use Tools > Combine Files to add PDFs, drag them into order, choose an output file, and combine them locally."),
        QCoreApplication::translate(
            "Tips", "Page rotation applies to every page currently selected."),
    };
}

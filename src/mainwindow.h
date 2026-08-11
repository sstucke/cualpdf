#pragma once

#include "appsettings.h"

#include <QMainWindow>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
class QFileSystemModel;
class QLabel;
class QSlider;
class QModelIndex;
class QPoint;
class QActionGroup;
class QAbstractItemDelegate;
QT_END_NAMESPACE

class FolderContentModel;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void openFolder();
    void goToParentFolder();
    void refreshCurrentFolder();
    void onTreeCurrentChanged(const QModelIndex &current);
    void onContentActivated(const QModelIndex &index);
    void onContentSelectionChanged();
    void onContentContextMenuRequested(const QPoint &pos);
    void onLocationEditReturnPressed();
    void onZoomSliderChanged(int value);
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void showAboutDialog();
    void onTabCloseRequested(int index);

    void setCurrentFolder(const QString &path);
    void updateStatusBarItemCount();
    void updateDetailsPanel(const QString &filePath);
    void clearDetailsPanel();
    void openPdfViewerTab(const QString &filePath);
    void openWithSystemDefault(const QString &filePath);
    void addRecentFile(const QString &filePath);
    void rebuildRecentFilesMenu();
    void setupViewModeMenu();
    void applyContentViewMode(AppSettings::ContentViewMode mode);

    Ui::MainWindow *ui;
    QFileSystemModel *treeModel;
    FolderContentModel *contentModel;
    QLabel *itemCountLabel;
    QSlider *zoomSlider;
    QActionGroup *viewModeActionGroup = nullptr;
    QAbstractItemDelegate *defaultContentDelegate = nullptr;
    QAbstractItemDelegate *detailsContentDelegate = nullptr;
    AppSettings appSettings;
    QString currentFolderPath;
    QStringList recentFiles;

    static constexpr int kDefaultThumbnailSize = 96;
    static constexpr int kMinIconSize = 32;
    static constexpr int kMaxIconSize = 256;
    static constexpr int kZoomStep = 16;
    static constexpr int kDetailsIconSize = 48;
    static constexpr int kCompactIconSize = 20;
    static constexpr int kMaxRecentFiles = 10;
    static constexpr int kGridCellPadding = 8;
    static constexpr int kGridTextHeight = 56;
};

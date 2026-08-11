#pragma once

#include <QMainWindow>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
class QFileSystemModel;
class QLabel;
class QSlider;
class QModelIndex;
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

    Ui::MainWindow *ui;
    QFileSystemModel *treeModel;
    FolderContentModel *contentModel;
    QLabel *itemCountLabel;
    QSlider *zoomSlider;
    QString currentFolderPath;

    static constexpr int kDefaultIconSize = 96;
    static constexpr int kMinIconSize = 32;
    static constexpr int kMaxIconSize = 256;
    static constexpr int kZoomStep = 16;
};

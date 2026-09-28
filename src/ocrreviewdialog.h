#pragma once

#include "ocrengine.h"
#include <QDialog>
#include <atomic>
#include <memory>

class QComboBox;
class QTableWidget;
class QLabel;
class QPushButton;
class QDialogButtonBox;

// Recognition is reviewed before any page in the open document is replaced.
class OcrReviewDialog final : public QDialog {
    Q_OBJECT
public:
    OcrReviewDialog(QVector<OcrPage> pages, const QVector<int> &indexes, QWidget *parent);
    QVector<OcrPage> pages() const { return m_pages; }
protected:
    void reject() override;
private:
    void loadPage();
    void showPreview();
    void rebuildPreview();
    QVector<OcrPage> m_pages;
    QVector<int> m_indexes;
    QComboBox *m_page;
    QComboBox *m_view;
    QTableWidget *m_table;
    QLabel *m_preview;
    QLabel *m_status;
    QPushButton *m_refresh;
    QDialogButtonBox *m_buttons;
    QImage m_result;
    bool m_loading = false;
    bool m_busy = false;
    bool m_closeAfterPreview = false;
    std::shared_ptr<std::atomic_bool> m_canceled;
};

#ifndef CUALPDF_PRINTDIALOG_H
#define CUALPDF_PRINTDIALOG_H

#include <QDialog>
#include <QPageSize>
#include <QRectF>
#include <memory>

class PdfDocument;
class QComboBox;
class QLabel;
class QPrinter;
class SheetPreview;
class QSpinBox;
class QCheckBox;
class QLineEdit;

class PrintDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PrintDialog(const std::shared_ptr<PdfDocument> &document, QWidget *parent = nullptr);
    ~PrintDialog() override;

private:
    struct Slot {
        QRectF paper;
        int pageIndex = -1;
    };

    void buildUi();
    void reloadPrinters();
    void syncPrinterFromUi();
    void syncUiFromPrinter();
    void editPrinterProperties();
    void refreshPreview();
    void paintSheet(QPainter *painter, const QRectF &target, int sheetIndex, int renderDpi) const;
    QVector<int> selectedPages() const;
    QSizeF paperPoints() const;
    QVector<QVector<Slot>> sheets() const;
    QString recommendedDuplex() const;
    void applyRecommendedDuplex();

    std::shared_ptr<PdfDocument> m_document;
    QPrinter *m_printer = nullptr;
    QComboBox *m_printerCombo = nullptr;
    QComboBox *m_dispositionCombo = nullptr;
    QComboBox *m_scaleCombo = nullptr;
    QComboBox *m_orientationCombo = nullptr;
    QComboBox *m_paperCombo = nullptr;
    QComboBox *m_duplexCombo = nullptr;
    QComboBox *m_colorCombo = nullptr;
    QSpinBox *m_copiesSpin = nullptr;
    QCheckBox *m_collateCheck = nullptr;
    QLineEdit *m_rangeEdit = nullptr;
    SheetPreview *m_preview = nullptr;
    QLabel *m_sheetLabel = nullptr;
    QLabel *m_hintLabel = nullptr;
    int m_sheet = 0;
    bool m_duplexTouched = false;
};

#endif

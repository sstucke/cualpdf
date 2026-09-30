#include "ocrreviewdialog.h"
#include "pdfdocument.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QHBoxLayout>

OcrReviewDialog::OcrReviewDialog(QVector<OcrPage> pages, const QVector<int> &indexes, QWidget *parent)
    : QDialog(parent), m_pages(std::move(pages)), m_indexes(indexes),
      m_canceled(std::make_shared<std::atomic_bool>(false))
{
    setWindowTitle(tr("Review editable OCR"));
    resize(1180, 820);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Review the text and proposed fonts. Unchecked lines remain in the original image. "
                              "Accepted lines become independent editable text boxes. Font matching and background repair are approximations."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *row = new QHBoxLayout;
    m_page = new QComboBox(this);
    for (const int index : indexes) m_page->addItem(tr("Page %1").arg(index + 1));
    m_view = new QComboBox(this);
    m_view->addItems({tr("Original + boxes"), tr("Editable result"), tr("Background only")});
    m_refresh = new QPushButton(tr("Update preview"), this);
    row->addWidget(m_page); row->addWidget(m_view); row->addWidget(m_refresh); row->addStretch();
    layout->addLayout(row);
    auto *splitter = new QSplitter(this);
    m_table = new QTableWidget(0, 4, splitter);
    m_table->setHorizontalHeaderLabels({tr("Use"), tr("Recognized text"), tr("Confidence"), tr("Substitute font")});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 42); m_table->setColumnWidth(2, 85); m_table->setColumnWidth(3, 165);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto *scroll = new QScrollArea(splitter);
    m_preview = new QLabel(scroll);
    m_preview->setAlignment(Qt::AlignCenter);
    scroll->setWidget(m_preview);
    splitter->addWidget(m_table); splitter->addWidget(scroll);
    splitter->setSizes({600, 550});
    layout->addWidget(splitter, 1);
    m_status = new QLabel(this); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply editable OCR"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &OcrReviewDialog::reject);
    connect(m_page, &QComboBox::currentIndexChanged, this, [this] { loadPage(); });
    connect(m_view, &QComboBox::currentIndexChanged, this, [this] { showPreview(); });
    connect(m_refresh, &QPushButton::clicked, this, [this] { rebuildPreview(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { showPreview(); });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_loading) return;
        auto &run = m_pages[m_page->currentIndex()].runs[item->row()];
        if (item->column() == 0) run.accepted = item->checkState() == Qt::Checked && !run.fontData.isEmpty();
        if (item->column() == 1) run.text = item->text();
        m_result = {}; m_pages[m_page->currentIndex()].background = {};
        showPreview();
        m_status->setText(tr("Preview needs updating. Changes will be rebuilt when applied."));
    });
    loadPage();
}

void OcrReviewDialog::loadPage()
{
    m_loading = true;
    m_result = {};
    const auto &page = m_pages[m_page->currentIndex()];
    m_table->setRowCount(page.runs.size());
    for (int i = 0; i < page.runs.size(); ++i) {
        const auto &run = page.runs[i];
        auto *use = new QTableWidgetItem;
        use->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        use->setCheckState(run.accepted ? Qt::Checked : Qt::Unchecked);
        if (run.fontData.isEmpty()) use->setFlags(Qt::ItemIsSelectable);
        m_table->setItem(i, 0, use);
        m_table->setItem(i, 1, new QTableWidgetItem(run.text));
        for (int column : {2, 3}) {
            auto *item = new QTableWidgetItem(column == 2 ? QString::number(run.confidence, 'f', 0) + '%'
                                                        : run.fontFamily);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (!run.accepted) item->setBackground(QColor(255, 223, 150));
            m_table->setItem(i, column, item);
        }
    }
    m_loading = false;
    m_view->setCurrentIndex(0);
    m_status->setText(tr("%1 text boxes. Check uncertain lines before applying.").arg(page.runs.size()));
    showPreview();
}

void OcrReviewDialog::showPreview()
{
    const auto &page = m_pages[m_page->currentIndex()];
    QImage image = m_view->currentIndex() == 0 ? page.source
                   : m_view->currentIndex() == 1 ? m_result : page.background;
    if (image.isNull()) {
        m_preview->setText(tr("Select Update preview to reconstruct this page."));
        m_preview->adjustSize(); return;
    }
    image = image.scaledToWidth(780, Qt::SmoothTransformation);
    if (m_view->currentIndex() == 0) {
        QPainter painter(&image);
        const double scale = double(image.width()) / page.source.width();
        painter.scale(scale, scale);
        for (int i = 0; i < page.runs.size(); ++i) {
            const auto &run = page.runs[i];
            painter.setPen(QPen(i == m_table->currentRow() ? Qt::red
                                    : run.accepted ? QColor(30, 115, 225) : QColor(200, 140, 0), 2 / scale));
            painter.drawRect(run.bounds);
        }
    }
    m_preview->setPixmap(QPixmap::fromImage(image)); m_preview->adjustSize();
}

void OcrReviewDialog::rebuildPreview()
{
    if (m_busy) return;
    m_busy = true; m_canceled->store(false);
    m_refresh->setEnabled(false); m_table->setEnabled(false); m_page->setEnabled(false);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    m_status->setText(tr("Reconstructing background and editable text…"));
    const QPointer<OcrReviewDialog> self(this);
    auto *thread = QThread::create([self, page = m_pages[m_page->currentIndex()], canceled = m_canceled]() mutable {
        QString error;
        QImage preview;
        try {
            if (OcrEngine::prepareBackground(&page, [canceled] { return canceled->load(); })) {
                const QByteArray archive = PdfDocument::createOcrPagesArchive({page}, &error);
                if (!archive.isEmpty()) { PdfDocument document(archive); preview = document.renderPage(0, 780); }
            }
        } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(qApp, [self, page, preview, error] {
            if (!self) return;
            self->m_busy = false;
            self->m_refresh->setEnabled(true); self->m_table->setEnabled(true); self->m_page->setEnabled(true);
            self->m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            self->m_pages[self->m_page->currentIndex()].background = page.background;
            self->m_result = preview;
            self->m_status->setText(error.isEmpty() ? tr("Preview updated.") : error);
            self->showPreview();
            if (self->m_closeAfterPreview) self->reject();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void OcrReviewDialog::reject()
{
    if (m_busy) { m_closeAfterPreview = true; m_canceled->store(true); return; }
    QDialog::reject();
}

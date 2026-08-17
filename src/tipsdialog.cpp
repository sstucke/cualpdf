#include "tipsdialog.h"

#include "tips.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kAutoCloseIntervalMs = 10000;
constexpr int kProgressUpdateIntervalMs = 50;
}

TipsDialog::TipsDialog(bool showAtStartup, bool autoCloseEnabled, QWidget *parent)
    : QDialog(parent)
    , m_tips(applicationTips())
    , m_tipLabel(new QLabel(this))
    , m_showAtStartupCheckBox(new QCheckBox(tr("Show tips at startup"), this))
    , m_autoCloseProgress(new QProgressBar(this))
    , m_autoCloseTimer(new QTimer(this))
    , m_progressTimer(new QTimer(this))
    , m_autoCloseEnabled(autoCloseEnabled)
{
    setWindowTitle(tr("Tips"));
    setModal(false);
    setMinimumWidth(480);

    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel(tr("Did you know?"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSize(headingFont.pointSize() + 2);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    m_tipLabel->setWordWrap(true);
    m_tipLabel->setMinimumHeight(72);
    m_tipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_tipLabel);

    m_showAtStartupCheckBox->setChecked(showAtStartup);
    connect(m_showAtStartupCheckBox, &QCheckBox::toggled,
            this, &TipsDialog::showAtStartupChanged);
    layout->addWidget(m_showAtStartupCheckBox);

    m_autoCloseProgress->setRange(0, kAutoCloseIntervalMs);
    m_autoCloseProgress->setTextVisible(false);
    m_autoCloseProgress->setFixedHeight(3);
    layout->addWidget(m_autoCloseProgress);

    m_autoCloseTimer->setSingleShot(true);
    connect(m_autoCloseTimer, &QTimer::timeout, this, &QDialog::close);
    m_progressTimer->setInterval(kProgressUpdateIntervalMs);
    connect(m_progressTimer, &QTimer::timeout,
            this, &TipsDialog::updateAutoCloseProgress);

    auto *buttonRow = new QHBoxLayout;
    auto *nextButton = new QPushButton(tr("Next Tip"), this);
    connect(nextButton, &QPushButton::clicked, this, &TipsDialog::showRandomTip);
    buttonRow->addWidget(nextButton);
    buttonRow->addStretch();

    auto *closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    closeButtons->button(QDialogButtonBox::Close)->setText(tr("Close"));
    connect(closeButtons, &QDialogButtonBox::rejected, this, &QDialog::close);
    buttonRow->addWidget(closeButtons);
    layout->addLayout(buttonRow);

    showRandomTip();
}

void TipsDialog::setShowAtStartup(bool enabled)
{
    const QSignalBlocker blocker(m_showAtStartupCheckBox);
    m_showAtStartupCheckBox->setChecked(enabled);
}

void TipsDialog::setAutoCloseEnabled(bool enabled)
{
    m_autoCloseEnabled = enabled;
    m_autoCloseProgress->setVisible(enabled);
    if (enabled)
        restartAutoClose();
    else {
        m_autoCloseTimer->stop();
        m_progressTimer->stop();
    }
}

void TipsDialog::showRandomTip()
{
    if (m_tips.isEmpty()) {
        m_tipLabel->setText(tr("No tips are available yet."));
        return;
    }

    int nextIndex = 0;
    if (m_tips.size() == 1) {
        nextIndex = 0;
    } else if (m_currentTipIndex < 0) {
        nextIndex = QRandomGenerator::global()->bounded(m_tips.size());
    } else {
        nextIndex = QRandomGenerator::global()->bounded(m_tips.size() - 1);
        if (nextIndex >= m_currentTipIndex)
            ++nextIndex;
    }

    m_currentTipIndex = nextIndex;
    m_tipLabel->setText(m_tips.at(m_currentTipIndex));
    restartAutoClose();
}

void TipsDialog::restartAutoClose()
{
    m_autoCloseProgress->setVisible(m_autoCloseEnabled);
    if (!m_autoCloseEnabled)
        return;

    m_remainingAutoCloseMs = kAutoCloseIntervalMs;
    resumeAutoClose();
}

void TipsDialog::pauseAutoClose()
{
    if (!m_autoCloseEnabled || !m_autoCloseTimer->isActive())
        return;
    m_remainingAutoCloseMs = qMax(
        0, m_remainingAutoCloseMs - static_cast<int>(m_autoCloseElapsed.elapsed()));
    m_autoCloseTimer->stop();
    m_progressTimer->stop();
    m_autoCloseProgress->setValue(m_remainingAutoCloseMs);
}

void TipsDialog::resumeAutoClose()
{
    if (!m_autoCloseEnabled || m_remainingAutoCloseMs <= 0)
        return;
    m_autoCloseElapsed.restart();
    m_autoCloseTimer->start(m_remainingAutoCloseMs);
    m_progressTimer->start();
    updateAutoCloseProgress();
}

void TipsDialog::updateAutoCloseProgress()
{
    const int elapsed = static_cast<int>(m_autoCloseElapsed.elapsed());
    m_autoCloseProgress->setValue(qMax(0, m_remainingAutoCloseMs - elapsed));
}

void TipsDialog::enterEvent(QEnterEvent *event)
{
    pauseAutoClose();
    QDialog::enterEvent(event);
}

void TipsDialog::leaveEvent(QEvent *event)
{
    resumeAutoClose();
    QDialog::leaveEvent(event);
}

void TipsDialog::keyPressEvent(QKeyEvent *event)
{
    restartAutoClose();
    QDialog::keyPressEvent(event);
}

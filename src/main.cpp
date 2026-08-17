#include "appversion.h"
#include "logger.h"
#include "mainwindow.h"
#include "pdfdocument.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

#include <exception>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setOrganizationName("cualpdf");
    QApplication::setApplicationName("cualpdf");
    QApplication::setApplicationVersion(QString::fromUtf8(AppVersion::value));

    Logger::install();

    // QTranslator::load tries the full locale name first (e.g. "es_AR"),
    // then progressively shorter forms (e.g. "es"), and simply fails
    // (leaving the English tr() source text in place) if none match.
    QTranslator translator;
    if (translator.load(QLocale::system(), QStringLiteral("cualpdf"), QStringLiteral("_"),
                         QStringLiteral(":/i18n")))
        QApplication::installTranslator(&translator);

    PdfDocument::initializeLibrary();

    int result = 0;
    try {
        // Scoped so MainWindow (and any PdfDocument instances it owns, e.g.
        // open viewer tabs) is destroyed before the library shuts down, even
        // when an exception unwinds out of this block.
        MainWindow window;
        window.show();
        result = application.exec();
    } catch (const std::exception &e) {
        qCritical() << "Unhandled exception:" << e.what();
        result = 1;
    } catch (...) {
        qCritical() << "Unhandled unknown exception";
        result = 1;
    }

    PdfDocument::shutdownLibrary();

    return result;
}

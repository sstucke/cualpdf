#include "mainwindow.h"
#include "pdfdocument.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setOrganizationName("cualpdf");
    QApplication::setApplicationName("cualpdf");
    QApplication::setApplicationVersion("0.1.0");

    // QTranslator::load tries the full locale name first (e.g. "es_AR"),
    // then progressively shorter forms (e.g. "es"), and simply fails
    // (leaving the English tr() source text in place) if none match.
    QTranslator translator;
    if (translator.load(QLocale::system(), QStringLiteral("cualpdf"), QStringLiteral("_"),
                         QStringLiteral(":/i18n")))
        QApplication::installTranslator(&translator);

    PdfDocument::initializeLibrary();

    int result = 0;
    {
        // Scoped so MainWindow (and any PdfDocument instances it owns, e.g.
        // open viewer tabs) is destroyed before the library shuts down.
        MainWindow window;
        window.show();
        result = application.exec();
    }

    PdfDocument::shutdownLibrary();

    return result;
}

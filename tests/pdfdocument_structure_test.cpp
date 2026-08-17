#include "pdfdocument.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>

namespace {
bool writeArchive(const QString &path, const QByteArray &archive)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(archive) == archive.size();
}

bool sizesMatch(const QVector<QSizeF> &actual, const QVector<QSizeF> &expected)
{
    if (actual.size() != expected.size())
        return false;
    for (int index = 0; index < actual.size(); ++index) {
        if (std::abs(actual[index].width() - expected[index].width()) > 0.01
            || std::abs(actual[index].height() - expected[index].height()) > 0.01) {
            return false;
        }
    }
    return true;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
        return 1;

    PdfDocument::initializeLibrary();
    int result = 0;
    {
        const QSizeF firstSize(100.0, 200.0);
        const QSizeF secondSize(200.0, 300.0);
        const QSizeF thirdSize(300.0, 400.0);
        const QByteArray firstArchive = PdfDocument::createBlankPageArchive(firstSize);
        const QByteArray secondArchive = PdfDocument::createBlankPageArchive(secondSize);
        const QByteArray thirdArchive = PdfDocument::createBlankPageArchive(thirdSize);
        const QString sourcePath = temporaryDirectory.filePath(QStringLiteral("source.pdf"));
        const QString secondSourcePath = temporaryDirectory.filePath(
            QStringLiteral("second.pdf"));
        if (firstArchive.isEmpty() || secondArchive.isEmpty() || thirdArchive.isEmpty()
            || !writeArchive(sourcePath, firstArchive)
            || !writeArchive(secondSourcePath, secondArchive)) {
            result = 2;
        } else {
            PdfDocument document(sourcePath);
            QVector<quint64> pageIds = {1};
            if (!document.isValid()
                || !document.restorePageStructure(pageIds, {1, 2}, {2}, secondArchive)) {
                result = 3;
            } else {
                pageIds = {1, 2};
                if (!document.restorePageStructure(pageIds, {1, 2, 3}, {3}, thirdArchive)) {
                    result = 4;
                } else {
                    pageIds = {1, 2, 3};
                    if (!document.restorePageStructure(pageIds, {3, 1, 2}, {}, {})) {
                        result = 5;
                    } else if (!sizesMatch(document.allPageSizes(),
                                           {thirdSize, firstSize, secondSize})) {
                        result = 6;
                    } else {
                        pageIds = {3, 1, 2};
                        if (!document.restorePageStructure(pageIds, {3, 2}, {}, {})) {
                            result = 7;
                        } else {
                            pageIds = {3, 2};
                            if (!document.restorePageStructure(
                                    pageIds, {3, 1, 2}, {1}, firstArchive)) {
                                result = 8;
                            } else if (!sizesMatch(document.allPageSizes(),
                                                   {thirdSize, firstSize, secondSize})) {
                                result = 9;
                            } else {
                                const QByteArray exported = document.exportPages({0, 2});
                                const QString exportedPath = temporaryDirectory.filePath(
                                    QStringLiteral("exported.pdf"));
                                if (exported.isEmpty()
                                    || !writeArchive(exportedPath, exported)) {
                                    result = 10;
                                } else {
                                    PdfDocument exportedDocument(exportedPath);
                                    if (!exportedDocument.isValid()
                                        || !sizesMatch(exportedDocument.allPageSizes(),
                                                       {thirdSize, secondSize})) {
                                        result = 11;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (result == 0) {
            const QString mergedPath = temporaryDirectory.filePath(
                QStringLiteral("merged.pdf"));
            QString failedInputPath;
            QString fileErrorMessage;
            if (!PdfDocument::mergeFiles({sourcePath, secondSourcePath}, mergedPath,
                                         &failedInputPath, &fileErrorMessage)) {
                result = 12;
            } else {
                {
                    PdfDocument mergedDocument(mergedPath);
                    if (!mergedDocument.isValid()
                        || !sizesMatch(mergedDocument.allPageSizes(),
                                       {firstSize, secondSize})) {
                        result = 13;
                    }
                }
                if (result == 0
                    && !PdfDocument::mergeFiles(
                        {secondSourcePath, sourcePath}, mergedPath,
                        &failedInputPath, &fileErrorMessage)) {
                    result = 14;
                } else if (result == 0) {
                    PdfDocument overwrittenDocument(mergedPath);
                    if (!overwrittenDocument.isValid()
                        || !sizesMatch(overwrittenDocument.allPageSizes(),
                                       {secondSize, firstSize})) {
                        result = 15;
                    }
                }
            }
        }
    }
    PdfDocument::shutdownLibrary();
    return result;
}

#include "imageenhancement.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace ImageEnhancement {

namespace {

// QImage::Format_RGB32 stores each pixel as bytes B, G, R, X in memory
// (little-endian 0xffRRGGBB word) — exactly OpenCV's native BGR(A) layout,
// so no channel swap is needed going in either direction.
cv::Mat qImageToBgrMat(const QImage &image)
{
    const QImage converted = image.convertToFormat(QImage::Format_RGB32);
    const cv::Mat bgrx(converted.height(), converted.width(), CV_8UC4,
                       const_cast<uchar *>(converted.constBits()),
                       static_cast<size_t>(converted.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(bgrx, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

QImage bgrMatToQImage(const cv::Mat &bgr)
{
    cv::Mat bgrx;
    cv::cvtColor(bgr, bgrx, cv::COLOR_BGR2BGRA);
    QImage image(bgrx.cols, bgrx.rows, QImage::Format_RGB32);
    for (int row = 0; row < bgrx.rows; ++row) {
        std::memcpy(image.scanLine(row), bgrx.ptr(row),
                    static_cast<size_t>(bgrx.cols) * 4);
    }
    return image;
}

// Where a single-channel 8-bit histogram crosses the given percentile
// ranks. Used for a percentile-based contrast stretch (matching
// numpy.percentile closely enough at 8-bit granularity).
void percentileBounds(const cv::Mat &channel, double lowPercentile,
                      double highPercentile, double &low, double &high)
{
    int bins = 256;
    const int channelIndex = 0;
    const float range[] = {0.0f, 256.0f};
    const float *ranges[] = {range};
    cv::Mat histogram;
    cv::calcHist(&channel, 1, &channelIndex, cv::Mat(), histogram, 1, &bins, ranges);

    const double total = static_cast<double>(channel.total());
    double cumulative = 0.0;
    low = 0.0;
    high = 255.0;
    bool lowFound = false;
    for (int bin = 0; bin < bins; ++bin) {
        cumulative += histogram.at<float>(bin);
        const double percentRank = cumulative / total * 100.0;
        if (!lowFound && percentRank >= lowPercentile) {
            low = bin;
            lowFound = true;
        }
        if (percentRank >= highPercentile) {
            high = bin;
            break;
        }
    }
}

// Pulls a Lab a/b (chroma) channel toward neutral gray (128) in proportion
// to `chromaKeep`, softening paper color casts without full desaturation.
cv::Mat desaturateChannel(const cv::Mat &channel, double chromaKeep)
{
    constexpr double kNeutral = 128.0;
    cv::Mat channelF;
    channel.convertTo(channelF, CV_32F);
    channelF = kNeutral + (channelF - kNeutral) * chromaKeep;
    cv::Mat result;
    channelF.convertTo(result, CV_8U);
    return result;
}

} // namespace

int clampAmount(int amount)
{
    return std::clamp(amount, 0, 100);
}

QImage aclararPapel(const QImage &page, int amount)
{
    return aclararPapel(page, amount, amount);
}

QImage aclararPapel(const QImage &page, int paperAmount, int textAmount)
{
    paperAmount = clampAmount(paperAmount);
    textAmount = clampAmount(textAmount);
    if ((paperAmount <= 0 && textAmount <= 0) || page.isNull())
        return page;

    const double paperStrength = paperAmount / 100.0;
    const double textStrength = textAmount / 100.0;
    const cv::Mat source = qImageToBgrMat(page);

    cv::Mat lab;
    cv::cvtColor(source, lab, cv::COLOR_BGR2Lab);
    std::array<cv::Mat, 3> labChannels;
    cv::split(lab, labChannels.data());
    const cv::Mat &lightness = labChannels[0];
    const cv::Mat &aChannel = labChannels[1];
    const cv::Mat &bChannel = labChannels[2];

    // Estimate the page's uneven illumination with a large median blur (a
    // rough "background" paper-shade map), then divide it out — this is
    // what flattens camera-shadow gradients that a plain scanner never has.
    const int height = lightness.rows;
    const int width = lightness.cols;
    int kernel = std::clamp(std::min(height, width) / 18, 15, 121);
    if (kernel % 2 == 0)
        ++kernel;

    cv::Mat background;
    cv::medianBlur(lightness, background, kernel);
    cv::max(background, 1, background);

    cv::Mat flattened;
    cv::divide(lightness, background, flattened, 255.0);

    double low = 0.0;
    double high = 255.0;
    percentileBounds(flattened, 1.0, 99.5, low, high);

    cv::Mat flattenedF;
    flattened.convertTo(flattenedF, CV_32F);
    if (high > low) {
        flattenedF = (flattenedF - low) * (255.0 / (high - low));
        cv::min(flattenedF, 255.0, flattenedF);
        cv::max(flattenedF, 0.0, flattenedF);
    }

    cv::Mat boosted;
    flattenedF.convertTo(boosted, CV_8U, 1.0 + 0.20 * textStrength,
                         10.0 + 30.0 * paperStrength);

    const cv::Ptr<cv::CLAHE> clahe =
        cv::createCLAHE(1.0 + 1.8 * textStrength, cv::Size(8, 8));
    cv::Mat local;
    clahe->apply(boosted, local);

    cv::Mat targetLightness;
    cv::addWeighted(boosted, 0.72, local, 0.28, 0.0, targetLightness);

    const double chromaKeep = 1.0 - 0.45 * paperStrength;
    const std::array<cv::Mat, 3> targetChannels{
        targetLightness,
        desaturateChannel(aChannel, chromaKeep),
        desaturateChannel(bChannel, chromaKeep),
    };
    cv::Mat targetLab;
    cv::merge(targetChannels.data(), targetChannels.size(), targetLab);
    cv::Mat targetBgr;
    cv::cvtColor(targetLab, targetBgr, cv::COLOR_Lab2BGR);

    cv::Mat result;
    const double blend = std::max(paperStrength, textStrength);
    cv::addWeighted(source, 1.0 - blend, targetBgr, blend, 0.0, result);

    return bgrMatToQImage(result);
}

QPair<int, int> ajustesAclaradoAutomatico(const QImage &page)
{
    if (page.isNull())
        return {};
    cv::Mat gray;
    cv::cvtColor(qImageToBgrMat(page), gray, cv::COLOR_BGR2GRAY);
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(gray, mean, deviation);
    const int paper = std::clamp(qRound((220.0 - mean[0]) * 0.9), 18, 85);
    const int text = std::clamp(qRound(58.0 - deviation[0] * 0.55), 15, 75);
    return {paper, text};
}

QImage invertirColores(const QImage &page)
{
    if (page.isNull())
        return {};
    QImage inverted = page.convertToFormat(QImage::Format_RGB32);
    inverted.invertPixels(QImage::InvertRgb);
    return inverted;
}

QMargins blackBorderMargins(const QImage &page)
{
    const QImage image = page.convertToFormat(QImage::Format_RGB32);
    if (image.width() < 8 || image.height() < 8)
        return {};
    const auto darkRow = [&](int y) {
        int dark = 0;
        for (int x = 0; x < image.width(); x += 2)
            if (qGray(image.pixel(x, y)) < 28)
                ++dark;
        return dark * 2 > image.width() / 2;
    };
    const auto darkColumn = [&](int x) {
        int dark = 0;
        for (int y = 0; y < image.height(); y += 2)
            if (qGray(image.pixel(x, y)) < 28)
                ++dark;
        return dark * 2 > image.height() / 2;
    };
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
    while (top < image.height() / 3 && darkRow(top))
        ++top;
    while (bottom < image.height() / 3 && darkRow(image.height() - 1 - bottom))
        ++bottom;
    while (left < image.width() / 3 && darkColumn(left))
        ++left;
    while (right < image.width() / 3 && darkColumn(image.width() - 1 - right))
        ++right;
    if (top + bottom + left + right < 4)
        return {};
    return QMargins(left, top, right, bottom);
}

QMargins whitePaperMargins(const QImage &page)
{
    const QImage image = page.convertToFormat(QImage::Format_RGB32);
    if (image.width() < 8 || image.height() < 8)
        return {};
    const auto rowHasInk = [&](int y) {
        int ink = 0;
        for (int x = 0; x < image.width(); x += 2)
            ink += qGray(image.pixel(x, y)) < 242;
        return ink >= std::max(2, image.width() / 160);
    };
    const auto columnHasInk = [&](int x) {
        int ink = 0;
        for (int y = 0; y < image.height(); y += 2)
            ink += qGray(image.pixel(x, y)) < 242;
        return ink >= std::max(2, image.height() / 160);
    };
    int top = 0, bottom = 0, left = 0, right = 0;
    while (top < image.height() / 3 && !rowHasInk(top)) ++top;
    while (bottom < image.height() / 3 && !rowHasInk(image.height() - 1 - bottom)) ++bottom;
    while (left < image.width() / 3 && !columnHasInk(left)) ++left;
    while (right < image.width() / 3 && !columnHasInk(image.width() - 1 - right)) ++right;
    if (top + bottom + left + right < 4)
        return {};
    constexpr int padding = 2;
    return QMargins(std::max(0, left - padding), std::max(0, top - padding),
                    std::max(0, right - padding), std::max(0, bottom - padding));
}

QImage mejorarEscaneo(const QImage &page, int level, bool whiteBackground, bool blackText,
                      bool blackAndWhite)
{
    if (page.isNull())
        return {};
    level = std::clamp(level, 0, 6);
    if (!whiteBackground && !blackText && !blackAndWhite)
        return page;
    const cv::Mat source = qImageToBgrMat(page);
    cv::Mat lab;
    cv::cvtColor(source, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels;
    cv::split(lab, channels);
    cv::Mat lightness = channels[0];
    const int kernel = std::max(15, std::min(151, std::min(source.rows, source.cols) / 18 | 1));
    const int oddKernel = kernel % 2 == 0 ? kernel + 1 : kernel;
    cv::Mat background;
    cv::medianBlur(lightness, background, oddKernel);
    background = cv::max(background, 1);
    cv::Mat flat;
    cv::divide(lightness, background, flat, 255.0);
    if (blackAndWhite) {
        cv::Mat binary;
        cv::threshold(flat, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        cv::Mat gray;
        cv::cvtColor(binary, gray, cv::COLOR_GRAY2BGR);
        return bgrMatToQImage(gray);
    }
    const double strength = level / 6.0;
    cv::Mat target = lightness.clone();
    if (whiteBackground) {
        const int threshold = 242 - level * 11;
        cv::Mat cleaned;
        cv::addWeighted(lightness, 1.0 - 0.72 * strength, flat, 0.72 * strength, 0, cleaned);
        target = cv::max(target, cleaned);
        target.setTo(255, flat >= threshold);
    }
    if (blackText) {
        cv::Mat gray;
        cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
        const int darkThreshold = 48 + level * 18;
        cv::Mat darkened;
        target.convertTo(darkened, CV_32F, 0.86 - 0.085 * level);
        cv::Mat darkened8;
        darkened.convertTo(darkened8, CV_8U);
        cv::Mat darker;
        cv::min(target, darkened8, darker);
        darker.copyTo(target, gray <= darkThreshold);
    }
    channels[0] = target;
    cv::Mat merged;
    cv::merge(channels, merged);
    cv::Mat bgr;
    cv::cvtColor(merged, bgr, cv::COLOR_Lab2BGR);
    return bgrMatToQImage(bgr);
}

QImage corregirPerspectiva(const QImage &page, const QVector<QPointF> &corners,
                           double targetRatio)
{
    if (page.isNull() || corners.size() != 4)
        return {};
    const cv::Mat source = qImageToBgrMat(page);
    const cv::Point2f src[4] = {
        {float(corners[0].x()), float(corners[0].y())},
        {float(corners[1].x()), float(corners[1].y())},
        {float(corners[2].x()), float(corners[2].y())},
        {float(corners[3].x()), float(corners[3].y())},
    };
    const double top = cv::norm(src[1] - src[0]);
    const double bottom = cv::norm(src[2] - src[3]);
    const double left = cv::norm(src[3] - src[0]);
    const double right = cv::norm(src[2] - src[1]);
    int width = std::max(2, int(std::max(top, bottom)));
    int height = std::max(2, int(std::max(left, right)));
    if (targetRatio > 0.0) {
        const double area = std::max(4.0, double(width) * height);
        width = std::max(2, qRound(std::sqrt(area * targetRatio)));
        height = std::max(2, qRound(width / targetRatio));
    }
    const cv::Point2f dst[4] = {
        {0, 0}, {float(width - 1), 0}, {float(width - 1), float(height - 1)}, {0, float(height - 1)},
    };
    cv::Mat equations(8, 8, CV_64F, cv::Scalar(0));
    cv::Mat values(8, 1, CV_64F, cv::Scalar(0));
    for (int i = 0; i < 4; ++i) {
        const double x = src[i].x;
        const double y = src[i].y;
        const double u = dst[i].x;
        const double v = dst[i].y;
        const int row = i * 2;
        equations.at<double>(row, 0) = x;
        equations.at<double>(row, 1) = y;
        equations.at<double>(row, 2) = 1;
        equations.at<double>(row, 6) = -u * x;
        equations.at<double>(row, 7) = -u * y;
        values.at<double>(row, 0) = u;
        equations.at<double>(row + 1, 3) = x;
        equations.at<double>(row + 1, 4) = y;
        equations.at<double>(row + 1, 5) = 1;
        equations.at<double>(row + 1, 6) = -v * x;
        equations.at<double>(row + 1, 7) = -v * y;
        values.at<double>(row + 1, 0) = v;
    }
    cv::Mat solution;
    if (!cv::solve(equations, values, solution))
        return {};
    cv::Mat transform(3, 3, CV_64F);
    for (int i = 0; i < 8; ++i)
        transform.at<double>(i / 3, i % 3) = solution.at<double>(i, 0);
    transform.at<double>(2, 2) = 1.0;
    cv::Mat warped;
    cv::warpPerspective(source, warped, transform, cv::Size(width, height),
                        cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return bgrMatToQImage(warped);
}

} // namespace ImageEnhancement

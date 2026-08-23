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
    amount = clampAmount(amount);
    if (amount <= 0 || page.isNull())
        return page;

    const double strength = amount / 100.0;
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
    flattenedF.convertTo(boosted, CV_8U, 1.0 + 0.12 * strength,
                         10.0 + 30.0 * strength);

    const cv::Ptr<cv::CLAHE> clahe =
        cv::createCLAHE(1.0 + 1.4 * strength, cv::Size(8, 8));
    cv::Mat local;
    clahe->apply(boosted, local);

    cv::Mat targetLightness;
    cv::addWeighted(boosted, 0.72, local, 0.28, 0.0, targetLightness);

    const double chromaKeep = 1.0 - 0.45 * strength;
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
    cv::addWeighted(source, 1.0 - strength, targetBgr, strength, 0.0, result);

    return bgrMatToQImage(result);
}

} // namespace ImageEnhancement

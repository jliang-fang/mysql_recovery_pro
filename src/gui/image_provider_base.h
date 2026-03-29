#ifndef IMAGE_PROVIDER_BASE_H
#define IMAGE_PROVIDER_BASE_H

#include <QImage>
#include <QString>

class ImageProviderBase {
public:
    virtual ~ImageProviderBase() = default;
    virtual QImage loadImage(const QString& filePath) = 0;
};

#endif // IMAGE_PROVIDER_BASE_H

#include "preview_controller.h"

#include <QImage>
#include <QPixmap>
#include <QDebug>

PreviewController::PreviewController(QObject *parent)
    : QObject(parent)
    , m_imageProvider(nullptr)
{
}

PreviewController::~PreviewController() = default;

void PreviewController::setImageProvider(ImageProviderBase *provider)
{
    if (m_imageProvider == provider)
        return;

    m_imageProvider = provider;
    emit imageProviderChanged();
}

QImage PreviewController::generatePreview(const QString &filePath, int width, int height) const
{
    if (!m_imageProvider || filePath.isEmpty()) {
        qWarning() << "PreviewController: No image provider or empty file path.";
        return QImage();
    }

    // Delegate actual image loading to the provider
    QImage image = m_imageProvider->loadImage(filePath);
    if (image.isNull()) {
        qWarning() << "PreviewController: Failed to load image from" << filePath;
        return QImage();
    }

    // Scale to requested dimensions while preserving aspect ratio
    if (width > 0 && height > 0) {
        image = image.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    return image;
}

QPixmap PreviewController::generatePreviewPixmap(const QString &filePath, int width, int height) const
{
    QImage image = generatePreview(filePath, width, height);
    return QPixmap::fromImage(image);
}

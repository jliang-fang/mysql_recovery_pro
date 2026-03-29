#ifndef PREVIEW_CONTROLLER_H
#define PREVIEW_CONTROLLER_H

#include <QObject>
#include <memory>

class ImageProviderBase;

class PreviewController : public QObject {
    Q_OBJECT

public:
    explicit PreviewController(QObject* parent = nullptr);
    ~PreviewController();

    void setImageProvider(ImageProviderBase* provider);
    Q_INVOKABLE QImage generatePreview(const QString& filePath, int width, int height) const;
    Q_INVOKABLE QPixmap generatePreviewPixmap(const QString& filePath, int width, int height) const;

signals:
    void imageProviderChanged();

private:
    ImageProviderBase* m_imageProvider = nullptr;
};

#endif // PREVIEW_CONTROLLER_H

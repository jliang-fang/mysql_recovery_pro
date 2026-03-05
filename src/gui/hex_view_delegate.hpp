#ifndef HEX_VIEW_DELEGATE_HPP
#define HEX_VIEW_DELEGATE_HPP

#include <QStyledItemDelegate>
#include <QModelIndex>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QString>

class HexViewDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit HexViewDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};

#endif // HEX_VIEW_DELEGATE_HPP

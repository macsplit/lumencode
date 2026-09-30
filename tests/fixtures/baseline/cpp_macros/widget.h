#pragma once

#include <QObject>

#define SHOP_EXPORT __attribute__((visibility("default")))
#define SHOP_PUBLIC(type) extern type

SHOP_PUBLIC(int) shop_version(void);
SHOP_PUBLIC(const char *) shop_name(int id);

class SHOP_EXPORT Widget : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int size READ size NOTIFY sizeChanged)
    QML_ELEMENT

public:
    explicit Widget(QObject *parent = nullptr);
    Q_INVOKABLE int size() const;

public Q_SLOTS:
    void grow(int by);

signals:
    void sizeChanged();

private:
    int m_size = 0;
};

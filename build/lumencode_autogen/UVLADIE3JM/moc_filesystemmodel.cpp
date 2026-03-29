/****************************************************************************
** Meta object code from reading C++ file 'filesystemmodel.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.13)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../src/filesystemmodel.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'filesystemmodel.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.13. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_FileSystemModel_t {
    QByteArrayData data[18];
    char stringdata0[208];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FileSystemModel_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FileSystemModel_t qt_meta_stringdata_FileSystemModel = {
    {
QT_MOC_LITERAL(0, 0, 15), // "FileSystemModel"
QT_MOC_LITERAL(1, 16, 21), // "visibleEntriesChanged"
QT_MOC_LITERAL(2, 38, 0), // ""
QT_MOC_LITERAL(3, 39, 15), // "rootPathChanged"
QT_MOC_LITERAL(4, 55, 11), // "setRootPath"
QT_MOC_LITERAL(5, 67, 4), // "path"
QT_MOC_LITERAL(6, 72, 14), // "toggleExpanded"
QT_MOC_LITERAL(7, 87, 10), // "isExpanded"
QT_MOC_LITERAL(8, 98, 14), // "visibleEntries"
QT_MOC_LITERAL(9, 113, 8), // "rootPath"
QT_MOC_LITERAL(10, 122, 5), // "Roles"
QT_MOC_LITERAL(11, 128, 8), // "NameRole"
QT_MOC_LITERAL(12, 137, 8), // "PathRole"
QT_MOC_LITERAL(13, 146, 9), // "IsDirRole"
QT_MOC_LITERAL(14, 156, 9), // "DepthRole"
QT_MOC_LITERAL(15, 166, 12), // "ExpandedRole"
QT_MOC_LITERAL(16, 179, 15), // "HasChildrenRole"
QT_MOC_LITERAL(17, 195, 12) // "FileTypeRole"

    },
    "FileSystemModel\0visibleEntriesChanged\0"
    "\0rootPathChanged\0setRootPath\0path\0"
    "toggleExpanded\0isExpanded\0visibleEntries\0"
    "rootPath\0Roles\0NameRole\0PathRole\0"
    "IsDirRole\0DepthRole\0ExpandedRole\0"
    "HasChildrenRole\0FileTypeRole"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FileSystemModel[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       2,   50, // properties
       1,   58, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   39,    2, 0x06 /* Public */,
       3,    0,   40,    2, 0x06 /* Public */,

 // methods: name, argc, parameters, tag, flags
       4,    1,   41,    2, 0x02 /* Public */,
       6,    1,   44,    2, 0x02 /* Public */,
       7,    1,   47,    2, 0x02 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,

 // methods: parameters
    QMetaType::Void, QMetaType::QString,    5,
    QMetaType::Void, QMetaType::QString,    5,
    QMetaType::Bool, QMetaType::QString,    5,

 // properties: name, type, flags
       8, QMetaType::QVariantList, 0x00495001,
       9, QMetaType::QString, 0x00495001,

 // properties: notify_signal_id
       0,
       1,

 // enums: name, alias, flags, count, data
      10,   10, 0x0,    7,   63,

 // enum data: key, value
      11, uint(FileSystemModel::NameRole),
      12, uint(FileSystemModel::PathRole),
      13, uint(FileSystemModel::IsDirRole),
      14, uint(FileSystemModel::DepthRole),
      15, uint(FileSystemModel::ExpandedRole),
      16, uint(FileSystemModel::HasChildrenRole),
      17, uint(FileSystemModel::FileTypeRole),

       0        // eod
};

void FileSystemModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FileSystemModel *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->visibleEntriesChanged(); break;
        case 1: _t->rootPathChanged(); break;
        case 2: _t->setRootPath((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->toggleExpanded((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: { bool _r = _t->isExpanded((*reinterpret_cast< const QString(*)>(_a[1])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (FileSystemModel::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FileSystemModel::visibleEntriesChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (FileSystemModel::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FileSystemModel::rootPathChanged)) {
                *result = 1;
                return;
            }
        }
    }
#ifndef QT_NO_PROPERTIES
    else if (_c == QMetaObject::ReadProperty) {
        auto *_t = static_cast<FileSystemModel *>(_o);
        (void)_t;
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast< QVariantList*>(_v) = _t->visibleEntries(); break;
        case 1: *reinterpret_cast< QString*>(_v) = _t->rootPath(); break;
        default: break;
        }
    } else if (_c == QMetaObject::WriteProperty) {
    } else if (_c == QMetaObject::ResetProperty) {
    }
#endif // QT_NO_PROPERTIES
}

QT_INIT_METAOBJECT const QMetaObject FileSystemModel::staticMetaObject = { {
    QMetaObject::SuperData::link<QAbstractItemModel::staticMetaObject>(),
    qt_meta_stringdata_FileSystemModel.data,
    qt_meta_data_FileSystemModel,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *FileSystemModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FileSystemModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_FileSystemModel.stringdata0))
        return static_cast<void*>(this);
    return QAbstractItemModel::qt_metacast(_clname);
}

int FileSystemModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QAbstractItemModel::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
#ifndef QT_NO_PROPERTIES
    else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    } else if (_c == QMetaObject::QueryPropertyDesignable) {
        _id -= 2;
    } else if (_c == QMetaObject::QueryPropertyScriptable) {
        _id -= 2;
    } else if (_c == QMetaObject::QueryPropertyStored) {
        _id -= 2;
    } else if (_c == QMetaObject::QueryPropertyEditable) {
        _id -= 2;
    } else if (_c == QMetaObject::QueryPropertyUser) {
        _id -= 2;
    }
#endif // QT_NO_PROPERTIES
    return _id;
}

// SIGNAL 0
void FileSystemModel::visibleEntriesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void FileSystemModel::rootPathChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE

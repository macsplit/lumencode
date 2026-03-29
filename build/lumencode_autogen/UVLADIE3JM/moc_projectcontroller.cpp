/****************************************************************************
** Meta object code from reading C++ file 'projectcontroller.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.13)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../src/projectcontroller.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'projectcontroller.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.13. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_ProjectController_t {
    QByteArrayData data[23];
    char stringdata0[335];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ProjectController_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ProjectController_t qt_meta_stringdata_ProjectController = {
    {
QT_MOC_LITERAL(0, 0, 17), // "ProjectController"
QT_MOC_LITERAL(1, 18, 15), // "rootPathChanged"
QT_MOC_LITERAL(2, 34, 0), // ""
QT_MOC_LITERAL(3, 35, 19), // "selectedPathChanged"
QT_MOC_LITERAL(4, 55, 23), // "selectedFileDataChanged"
QT_MOC_LITERAL(5, 79, 21), // "selectedSymbolChanged"
QT_MOC_LITERAL(6, 101, 14), // "lastOpenedPath"
QT_MOC_LITERAL(7, 116, 11), // "setRootPath"
QT_MOC_LITERAL(8, 128, 4), // "path"
QT_MOC_LITERAL(9, 133, 10), // "selectPath"
QT_MOC_LITERAL(10, 144, 12), // "selectSymbol"
QT_MOC_LITERAL(11, 157, 5), // "index"
QT_MOC_LITERAL(12, 163, 18), // "selectSymbolByData"
QT_MOC_LITERAL(13, 182, 6), // "symbol"
QT_MOC_LITERAL(14, 189, 10), // "pickFolder"
QT_MOC_LITERAL(15, 200, 21), // "restoreLastOpenedPath"
QT_MOC_LITERAL(16, 222, 15), // "fileSystemModel"
QT_MOC_LITERAL(17, 238, 8), // "rootPath"
QT_MOC_LITERAL(18, 247, 12), // "selectedPath"
QT_MOC_LITERAL(19, 260, 20), // "selectedRelativePath"
QT_MOC_LITERAL(20, 281, 16), // "selectedFileData"
QT_MOC_LITERAL(21, 298, 14), // "selectedSymbol"
QT_MOC_LITERAL(22, 313, 21) // "selectedSymbolMembers"

    },
    "ProjectController\0rootPathChanged\0\0"
    "selectedPathChanged\0selectedFileDataChanged\0"
    "selectedSymbolChanged\0lastOpenedPath\0"
    "setRootPath\0path\0selectPath\0selectSymbol\0"
    "index\0selectSymbolByData\0symbol\0"
    "pickFolder\0restoreLastOpenedPath\0"
    "fileSystemModel\0rootPath\0selectedPath\0"
    "selectedRelativePath\0selectedFileData\0"
    "selectedSymbol\0selectedSymbolMembers"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ProjectController[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      11,   14, // methods
       7,   88, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   69,    2, 0x06 /* Public */,
       3,    0,   70,    2, 0x06 /* Public */,
       4,    0,   71,    2, 0x06 /* Public */,
       5,    0,   72,    2, 0x06 /* Public */,

 // methods: name, argc, parameters, tag, flags
       6,    0,   73,    2, 0x02 /* Public */,
       7,    1,   74,    2, 0x02 /* Public */,
       9,    1,   77,    2, 0x02 /* Public */,
      10,    1,   80,    2, 0x02 /* Public */,
      12,    1,   83,    2, 0x02 /* Public */,
      14,    0,   86,    2, 0x02 /* Public */,
      15,    0,   87,    2, 0x02 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

 // methods: parameters
    QMetaType::QString,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void, QMetaType::Int,   11,
    QMetaType::Void, QMetaType::QVariantMap,   13,
    QMetaType::QString,
    QMetaType::Bool,

 // properties: name, type, flags
      16, QMetaType::QObjectStar, 0x00095401,
      17, QMetaType::QString, 0x00495103,
      18, QMetaType::QString, 0x00495001,
      19, QMetaType::QString, 0x00495001,
      20, QMetaType::QVariantMap, 0x00495001,
      21, QMetaType::QVariantMap, 0x00495001,
      22, QMetaType::QVariantList, 0x00495001,

 // properties: notify_signal_id
       0,
       0,
       1,
       1,
       2,
       3,
       3,

       0        // eod
};

void ProjectController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ProjectController *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->rootPathChanged(); break;
        case 1: _t->selectedPathChanged(); break;
        case 2: _t->selectedFileDataChanged(); break;
        case 3: _t->selectedSymbolChanged(); break;
        case 4: { QString _r = _t->lastOpenedPath();
            if (_a[0]) *reinterpret_cast< QString*>(_a[0]) = std::move(_r); }  break;
        case 5: _t->setRootPath((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 6: _t->selectPath((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 7: _t->selectSymbol((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 8: _t->selectSymbolByData((*reinterpret_cast< const QVariantMap(*)>(_a[1]))); break;
        case 9: { QString _r = _t->pickFolder();
            if (_a[0]) *reinterpret_cast< QString*>(_a[0]) = std::move(_r); }  break;
        case 10: { bool _r = _t->restoreLastOpenedPath();
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ProjectController::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ProjectController::rootPathChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ProjectController::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ProjectController::selectedPathChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (ProjectController::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ProjectController::selectedFileDataChanged)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (ProjectController::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ProjectController::selectedSymbolChanged)) {
                *result = 3;
                return;
            }
        }
    }
#ifndef QT_NO_PROPERTIES
    else if (_c == QMetaObject::ReadProperty) {
        auto *_t = static_cast<ProjectController *>(_o);
        (void)_t;
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast< QObject**>(_v) = _t->fileSystemModel(); break;
        case 1: *reinterpret_cast< QString*>(_v) = _t->rootPath(); break;
        case 2: *reinterpret_cast< QString*>(_v) = _t->selectedPath(); break;
        case 3: *reinterpret_cast< QString*>(_v) = _t->selectedRelativePath(); break;
        case 4: *reinterpret_cast< QVariantMap*>(_v) = _t->selectedFileData(); break;
        case 5: *reinterpret_cast< QVariantMap*>(_v) = _t->selectedSymbol(); break;
        case 6: *reinterpret_cast< QVariantList*>(_v) = _t->selectedSymbolMembers(); break;
        default: break;
        }
    } else if (_c == QMetaObject::WriteProperty) {
        auto *_t = static_cast<ProjectController *>(_o);
        (void)_t;
        void *_v = _a[0];
        switch (_id) {
        case 1: _t->setRootPath(*reinterpret_cast< QString*>(_v)); break;
        default: break;
        }
    } else if (_c == QMetaObject::ResetProperty) {
    }
#endif // QT_NO_PROPERTIES
}

QT_INIT_METAOBJECT const QMetaObject ProjectController::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_ProjectController.data,
    qt_meta_data_ProjectController,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ProjectController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ProjectController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ProjectController.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ProjectController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 11)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 11;
    }
#ifndef QT_NO_PROPERTIES
    else if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::QueryPropertyDesignable) {
        _id -= 7;
    } else if (_c == QMetaObject::QueryPropertyScriptable) {
        _id -= 7;
    } else if (_c == QMetaObject::QueryPropertyStored) {
        _id -= 7;
    } else if (_c == QMetaObject::QueryPropertyEditable) {
        _id -= 7;
    } else if (_c == QMetaObject::QueryPropertyUser) {
        _id -= 7;
    }
#endif // QT_NO_PROPERTIES
    return _id;
}

// SIGNAL 0
void ProjectController::rootPathChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void ProjectController::selectedPathChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void ProjectController::selectedFileDataChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void ProjectController::selectedSymbolChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE

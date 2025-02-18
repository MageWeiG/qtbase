// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only

#include "qwasmclipboard.h"
#include "qwasmwindow.h"
#include "qwasmstring.h"
#include <private/qstdweb_p.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <QCoreApplication>
#include <qpa/qwindowsysteminterface.h>
#include <QBuffer>
#include <QString>

QT_BEGIN_NAMESPACE
using namespace emscripten;

static void commonCopyEvent(val event)
{
    QMimeData *_mimes = QWasmIntegration::get()->getWasmClipboard()->mimeData(QClipboard::Clipboard);
    if (!_mimes)
      return;

    // doing it this way seems to sanitize the text better that calling data() like down below
    if (_mimes->hasText()) {
        event["clipboardData"].call<void>("setData", val("text/plain")
                                          ,  QWasmString::fromQString(_mimes->text()));
    }
    if (_mimes->hasHtml()) {
        event["clipboardData"].call<void>("setData", val("text/html")
                                          ,   QWasmString::fromQString(_mimes->html()));
    }

    for (auto mimetype : _mimes->formats()) {
        if (mimetype.contains("text/"))
            continue;
        QByteArray ba = _mimes->data(mimetype);
        if (!ba.isEmpty())
            event["clipboardData"].call<void>("setData", QWasmString::fromQString(mimetype)
                                              , val(ba.constData()));
    }

    event.call<void>("preventDefault");
    QWasmIntegration::get()->getWasmClipboard()->m_isListener = false;
}

static void qClipboardCutTo(val event)
{
    QWasmIntegration::get()->getWasmClipboard()->m_isListener = true;
    if (!QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi) {
        // Send synthetic Ctrl+X to make the app cut data to Qt's clipboard
         QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
                     0, QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, "X");
   }

    commonCopyEvent(event);
}

static void qClipboardCopyTo(val event)
{
    QWasmIntegration::get()->getWasmClipboard()->m_isListener = true;

    if (!QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi) {
        // Send synthetic Ctrl+C to make the app copy data to Qt's clipboard
            QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
                        0, QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, "C");
    }
    commonCopyEvent(event);
}

static void qClipboardPasteTo(val dataTransfer)
{
    QWasmIntegration::get()->getWasmClipboard()->m_isListener = true;
    val clipboardData = dataTransfer["clipboardData"];
    val types = clipboardData["types"];
    int typesCount = types["length"].as<int>();
    std::string stdMimeFormat;
    QMimeData *mMimeData = new QMimeData;
    for (int i = 0; i < typesCount; i++) {
        stdMimeFormat = types[i].as<std::string>();
        QString mimeFormat =  QString::fromStdString(stdMimeFormat);
        if (mimeFormat.contains("STRING", Qt::CaseSensitive) || mimeFormat.contains("TEXT", Qt::CaseSensitive))
            continue;

        if (mimeFormat.contains("text")) {
// also "text/plain;charset=utf-8"
// "UTF8_STRING" "MULTIPLE"
            val mimeData = clipboardData.call<val>("getData", val(stdMimeFormat)); // as DataTransfer

            const QString qstr = QWasmString::toQString(mimeData);

            if (qstr.length() > 0) {
                if (mimeFormat.contains("text/html")) {
                    mMimeData->setHtml(qstr);
                } else if (mimeFormat.isEmpty() || mimeFormat.contains("text/plain")) {
                    mMimeData->setText(qstr); // the type can be empty
                } else {
                    mMimeData->setData(mimeFormat, qstr.toLocal8Bit());}
            }
        } else {
            val items = clipboardData["items"];

            int itemsCount = items["length"].as<int>();
            // handle data
            for (int i = 0; i < itemsCount; i++) {
                val item = items[i];
                val clipboardFile = item.call<emscripten::val>("getAsFile"); // string kind is handled above
                if (clipboardFile.isUndefined() || item["kind"].as<std::string>() == "string" ) {
                    continue;
                }
                qstdweb::File file(clipboardFile);

                mimeFormat =  QString::fromStdString(file.type());
                QByteArray fileContent;
                fileContent.resize(file.size());

                file.stream(fileContent.data(), [=]() {
                    if (!fileContent.isEmpty()) {

                        if (mimeFormat.contains("image")) {
                            QImage image;
                            image.loadFromData(fileContent, nullptr);
                            mMimeData->setImageData(image);
                        } else {
                            mMimeData->setData(mimeFormat,fileContent.data());
                        }
                        QWasmClipboard::qWasmClipboardPaste(mMimeData);
                    }
                });
            } // next item
        }
    }
    QWasmClipboard::qWasmClipboardPaste(mMimeData);
    QWasmIntegration::get()->getWasmClipboard()->m_isListener = false;
}

EMSCRIPTEN_BINDINGS(qtClipboardModule) {
    function("qtClipboardCutTo", &qClipboardCutTo);
    function("qtClipboardCopyTo", &qClipboardCopyTo);
    function("qtClipboardPasteTo", &qClipboardPasteTo);
}

QWasmClipboard::QWasmClipboard() :
    isPaste(false),
    m_isListener(false)
{
    val clipboard = val::global("navigator")["clipboard"];

    const bool hasPermissionsApi = !val::global("navigator")["permissions"].isUndefined();
    hasClipboardApi = !clipboard.isUndefined() && !clipboard["readText"].isUndefined();

    if (hasClipboardApi && hasPermissionsApi)
        initClipboardPermissions();
}

QWasmClipboard::~QWasmClipboard()
{
}

QMimeData *QWasmClipboard::mimeData(QClipboard::Mode mode)
{
    if (mode != QClipboard::Clipboard)
        return nullptr;

    return QPlatformClipboard::mimeData(mode);
}

void QWasmClipboard::setMimeData(QMimeData *mimeData, QClipboard::Mode mode)
{
    QPlatformClipboard::setMimeData(mimeData, mode);
    // handle setText/ setData programmatically
    if (!isPaste) {
        if (hasClipboardApi) {
            writeToClipboardApi();
        } else if (!m_isListener) {
            writeToClipboard(mimeData);
        }
    }
    isPaste = false;
}

bool QWasmClipboard::processKeyboard(const QWasmEventTranslator::TranslatedEvent &event,
                                     const QFlags<Qt::KeyboardModifier> &modifiers)
{
    // Clipboard path: cut/copy/paste are handled by clipboard event or direct clipboard
    // access.
    if (hasClipboardApi)
        return false;
    if (event.type != QEvent::KeyPress || !modifiers.testFlag(Qt::ControlModifier)
        || (event.key != Qt::Key_C && event.key != Qt::Key_V && event.key != Qt::Key_X)) {
        return false;
    }
    isPaste = event.key == Qt::Key_V;
    return true;
}

bool QWasmClipboard::supportsMode(QClipboard::Mode mode) const
{
    return mode == QClipboard::Clipboard;
}

bool QWasmClipboard::ownsMode(QClipboard::Mode mode) const
{
    Q_UNUSED(mode);
    return false;
}

void QWasmClipboard::qWasmClipboardPaste(QMimeData *mData)
{
    QWasmIntegration::get()->clipboard()->setMimeData(mData, QClipboard::Clipboard);

    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
                0, QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier, "V");
}

void QWasmClipboard::initClipboardPermissions()
{
    val permissions = val::global("navigator")["permissions"];

    qstdweb::Promise::make(permissions, "query", { .catchFunc = [](emscripten::val) {} }, ([]() {
                               val readPermissionsMap = val::object();
                               readPermissionsMap.set("name", val("clipboard-read"));
                               return readPermissionsMap;
                           })());
    qstdweb::Promise::make(permissions, "query", { .catchFunc = [](emscripten::val) {} }, ([]() {
                               val readPermissionsMap = val::object();
                               readPermissionsMap.set("name", val("clipboard-write"));
                               return readPermissionsMap;
                           })());
}

void QWasmClipboard::installEventHandlers(const emscripten::val &canvas)
{
    emscripten::val cContext = val::undefined();
    emscripten::val isChromium = val::global("window")["chrome"];
   if (!isChromium.isUndefined()) {
        cContext = val::global("document");
   } else {
       cContext = canvas;
   }
    // Fallback path for browsers which do not support direct clipboard access
    cContext.call<void>("addEventListener", val("cut"),
                        val::module_property("qtClipboardCutTo"), true);
    cContext.call<void>("addEventListener", val("copy"),
                        val::module_property("qtClipboardCopyTo"), true);
    cContext.call<void>("addEventListener", val("paste"),
                        val::module_property("qtClipboardPasteTo"), true);
}

void QWasmClipboard::writeToClipboardApi()
{
    if (!QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi)
        return;

    // copy event
    // browser event handler detected ctrl c if clipboard API
    // or Qt call from keyboard event handler

    QMimeData *_mimes = QWasmIntegration::get()->getWasmClipboard()->mimeData(QClipboard::Clipboard);
    if (!_mimes)
        return;

    emscripten::val clipboardWriteArray = emscripten::val::array();
    QByteArray ba;

    for (auto mimetype : _mimes->formats()) {
        // we need to treat binary and text differently, as the blob method below
        // fails for text mimetypes
        // ignore text types

        if (mimetype.contains("STRING", Qt::CaseSensitive) || mimetype.contains("TEXT", Qt::CaseSensitive))
            continue;

        if (_mimes->hasHtml()) { // prefer html over text
            ba = _mimes->html().toLocal8Bit();
            // force this mime
            mimetype = "text/html";
        } else if (mimetype.contains("text/plain")) {
            ba = _mimes->text().toLocal8Bit();
        } else if (mimetype.contains("image")) {
            QImage img = qvariant_cast<QImage>( _mimes->imageData());
            QBuffer buffer(&ba);
            buffer.open(QIODevice::WriteOnly);
            img.save(&buffer, "PNG");
            mimetype = "image/png"; // chrome only allows png
            // clipboard error "NotAllowedError" "Type application/x-qt-image not supported on write."
            // safari silently fails
            // so we use png internally for now
        } else {
            // DATA
            ba = _mimes->data(mimetype);
        }
        // Create file data Blob

        const char *content = ba.data();
        int dataLength = ba.length();
        if (dataLength < 1) {
            qDebug() << "no content found";
            return;
        }

        emscripten::val document = emscripten::val::global("document");
        emscripten::val window = emscripten::val::global("window");

        emscripten::val fileContentView =
                emscripten::val(emscripten::typed_memory_view(dataLength, content));
        emscripten::val fileContentCopy = emscripten::val::global("ArrayBuffer").new_(dataLength);
        emscripten::val fileContentCopyView =
                emscripten::val::global("Uint8Array").new_(fileContentCopy);
        fileContentCopyView.call<void>("set", fileContentView);

        emscripten::val contentArray = emscripten::val::array();
        contentArray.call<void>("push", fileContentCopyView);

        // we have a blob, now create a ClipboardItem
        emscripten::val type = emscripten::val::array();
        type.set("type", val(QWasmString::fromQString(mimetype)));

        emscripten::val contentBlob = emscripten::val::global("Blob").new_(contentArray, type);

        emscripten::val clipboardItemObject = emscripten::val::object();
        clipboardItemObject.set(val(QWasmString::fromQString(mimetype)), contentBlob);

        val clipboardItemData = val::global("ClipboardItem").new_(clipboardItemObject);

        clipboardWriteArray.call<void>("push", clipboardItemData);

        // Clipboard write is only supported with one ClipboardItem at the moment
        // but somehow this still works?
        // break;
    }

    val navigator = val::global("navigator");

    qstdweb::Promise::make(
        navigator["clipboard"], "write",
        {
            .catchFunc = [](emscripten::val error) {
                qWarning() << "clipboard error"
                    << QString::fromStdString(error["name"].as<std::string>())
                    << QString::fromStdString(error["message"].as<std::string>());
            }
        },
        clipboardWriteArray);
}

void QWasmClipboard::writeToClipboard(const QMimeData *data)
{
    Q_UNUSED(data)
    // this works for firefox, chrome by generating
    // copy event, but not safari
    // execCommand has been deemed deprecated in the docs, but browsers do not seem
    // interested in removing it. There is no replacement, so we use it here.
    val document = val::global("document");
    document.call<val>("execCommand", val("copy"));
}
QT_END_NAMESPACE

#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QWindow>

// QtGui exports the window-system entry points independently of QtTest. Keep
// this transitional helper limited to the legacy window-input tests.
QT_BEGIN_NAMESPACE
Q_GUI_EXPORT void qt_handleMouseEvent(QWindow*, const QPointF&, const QPointF&,
                                      Qt::MouseButtons, Qt::MouseButton,
                                      QEvent::Type, Qt::KeyboardModifiers, int);
Q_GUI_EXPORT void qt_handleKeyEvent(QWindow*, QEvent::Type, int,
                                    Qt::KeyboardModifiers, const QString&, bool, ushort);
QT_END_NAMESPACE

namespace agent_ctest {
inline void clickWindow(QWindow* window, QPoint position)
{
    static int timestamp = 0;
    ++timestamp;
    if (position.isNull()) position = QPoint(window->width() / 2, window->height() / 2);
    const QPointF local = position;
    const QPointF global = window->mapToGlobal(position);
    QPointer<QWindow> guarded(window);
    QT_PREPEND_NAMESPACE(qt_handleMouseEvent)(window, local, global, Qt::LeftButton,
        Qt::LeftButton, QEvent::MouseButtonPress, Qt::NoModifier, timestamp);
    if (guarded) {
        QT_PREPEND_NAMESPACE(qt_handleMouseEvent)(guarded, local, global, Qt::NoButton,
            Qt::LeftButton, QEvent::MouseButtonRelease, Qt::NoModifier, timestamp);
    }
    timestamp += 500; // Separate independent clicks from a double click.
    QCoreApplication::processEvents();
}

inline void pressEscape(QWindow* window)
{
    QPointer<QWindow> guarded(window);
    QT_PREPEND_NAMESPACE(qt_handleKeyEvent)(window, QEvent::KeyPress, Qt::Key_Escape,
        Qt::NoModifier, QString(), false, 1);
    QCoreApplication::processEvents();
    if (guarded) {
        QT_PREPEND_NAMESPACE(qt_handleKeyEvent)(guarded, QEvent::KeyRelease, Qt::Key_Escape,
            Qt::NoModifier, QString(), false, 1);
        QCoreApplication::processEvents();
    }
}
} // namespace agent_ctest

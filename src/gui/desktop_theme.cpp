#include "desktop_theme.h"

#include <cstdio>

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>

namespace {

// The portal's org.freedesktop.appearance color-scheme: 0 no preference,
// 1 prefer dark, 2 prefer light; -1 if there's no portal to ask (no
// desktop session, e.g. over SSH) or it doesn't know the setting.
int portalColorScheme() {
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
                                                       "/org/freedesktop/portal/desktop",
                                                       "org.freedesktop.portal.Settings", "Read");
    call << QString("org.freedesktop.appearance") << QString("color-scheme");
    QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 1000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return -1;
    // Read() wraps the value in a variant, and some portal versions wrap
    // it twice.
    QVariant value = reply.arguments().first();
    while (value.canConvert<QDBusVariant>()) value = value.value<QDBusVariant>().variant();
    bool ok = false;
    int scheme = value.toInt(&ok);
    return ok ? scheme : -1;
}

QPalette darkPalette() {
    const QColor window(0x35, 0x35, 0x35), base(0x2a, 0x2a, 0x2a), text(0xee, 0xee, 0xee);
    const QColor disabled(0x80, 0x80, 0x80), highlight(0x2a, 0x82, 0xda);
    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light, window.lighter(150));
    p.setColor(QPalette::Midlight, window.lighter(125));
    p.setColor(QPalette::Mid, window.darker(125));
    p.setColor(QPalette::Dark, window.darker(150));
    p.setColor(QPalette::Shadow, Qt::black);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    p.setColor(QPalette::Disabled, QPalette::Base, window);
    return p;
}

// Qt 6.4's Fusion outlines check boxes and radio buttons in the window
// colour darkened by 40% -- on this palette #262626 against a #2a2a2a box,
// next to invisible (6.5's Fusion lightens it in dark mode instead). Draw
// those indicators as if on a light grey window, for a mid-grey outline.
class DarkIndicatorStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override {
        if (element == PE_IndicatorCheckBox || element == PE_IndicatorRadioButton) {
            QStyleOption *copy = nullptr;
            if (auto *button = qstyleoption_cast<const QStyleOptionButton *>(option))
                copy = new QStyleOptionButton(*button);
            else if (auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option))
                copy = new QStyleOptionViewItem(*item);
            else
                copy = new QStyleOption(*option);
            copy->palette.setColor(QPalette::Window, QColor(0xb0, 0xb0, 0xb0));
            QProxyStyle::drawPrimitive(element, copy, painter, widget);
            delete copy;
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

} // namespace

void followDesktopColorScheme(QApplication &app) {
    bool qtDark = app.palette().color(QPalette::Window).lightness() < 128;
    if (qtDark) return; // Qt already follows the desktop (Qt 6.5+)
    if (portalColorScheme() != 1) return;
    app.setStyle(new DarkIndicatorStyle(QStyleFactory::create("Fusion")));
    app.setPalette(darkPalette());
    std::fprintf(stderr, "dv3kclient: desktop prefers dark, using a dark palette\n");
}

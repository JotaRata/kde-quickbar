/*
    Application name label for QuickBar — opens the window actions menu
*/
import QtQuick
import QtQuick.Controls

import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

AbstractButton {
    id: root

    property int fontSize: 0
    property string fontFamily: ""
    property int fontWeight: 0
    property int hoverCornerRadius: 4

    signal activated()

    hoverEnabled: true
    onPressed: activated()

    enum State { Rest, Hover, Down }

    readonly property int menuState: {
        if (down) {
            return AppNameLabel.State.Down
        }
        if (hovered) {
            return AppNameLabel.State.Hover
        }
        return AppNameLabel.State.Rest
    }

    // Same padding as MenuDelegate (Kirigami.Units.smallSpacing): keeps the
    // hover background the same height and text inset as the menu-entry
    // buttons.
    padding: 0
    topPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing
    leftPadding: Kirigami.Units.smallSpacing
    rightPadding: Kirigami.Units.smallSpacing

    Accessible.description: i18nc("@info:usagetip", "Open the window actions menu")

    background: Rectangle {
        anchors.fill: parent
        radius: root.menuState !== AppNameLabel.State.Rest ? root.hoverCornerRadius : 0
        color: {
            if (root.menuState === AppNameLabel.State.Down) {
                return Kirigami.Theme.highlightColor
            }
            if (root.menuState === AppNameLabel.State.Hover) {
                return Kirigami.ColorUtils.linearInterpolation(
                    Kirigami.Theme.backgroundColor,
                    Kirigami.Theme.hoverColor,
                    0.35)
            }
            return "transparent"
        }
    }

    contentItem: PC3.Label {
        text: root.text
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignLeft
        elide: Text.ElideRight
        font.family: root.fontFamily || Kirigami.Theme.defaultFont.family
        font.pointSize: root.fontSize > 0 ? root.fontSize : Kirigami.Theme.defaultFont.pointSize
        font.weight: root.fontWeight > 0 ? root.fontWeight : Kirigami.Theme.defaultFont.weight
        color: root.menuState !== AppNameLabel.State.Rest
            ? Kirigami.Theme.highlightedTextColor
            : Kirigami.Theme.textColor
    }
}

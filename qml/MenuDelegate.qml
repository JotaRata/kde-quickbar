/*
    Configurable menu bar button delegate for QuickBar
    Based on KDE Plasma Global Menu MenuDelegate (GPL-2.0-or-later)
*/
import QtQuick
import QtQuick.Controls

import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

AbstractButton {
    id: controlRoot

    property bool menuIsOpen: false
    property bool hoverOpensMenu: true
    property int hoverCornerRadius: 4
    property int fontSize: 0
    property string fontFamily: ""
    property int fontWeight: 0
    property string textColor: ""
    property string hoverTextColor: ""
    property string iconName: ""

    readonly property color effectiveTextColor: {
        const useHover = controlRoot.menuState !== MenuDelegate.State.Rest
        const custom = useHover ? controlRoot.hoverTextColor : controlRoot.textColor
        if (custom && custom.length > 0) {
            return custom
        }
        return useHover ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
    }

    signal activated()

    hoverEnabled: true
    onHoveredChanged: {
        if (hoverOpensMenu && hovered && menuIsOpen) {
            activated()
        }
    }
    onPressed: activated()

    enum State { Rest, Hover, Down }

    property int menuState: {
        if (down) {
            return MenuDelegate.State.Down
        }
        if (hovered && !menuIsOpen) {
            return MenuDelegate.State.Hover
        }
        return MenuDelegate.State.Rest
    }

    readonly property bool showHoverBackground: controlRoot.menuState !== MenuDelegate.State.Rest

    Kirigami.MnemonicData.controlType: Kirigami.MnemonicData.SecondaryControl
    Kirigami.MnemonicData.label: text

    topPadding: Kirigami.Units.smallSpacing
    leftPadding: Kirigami.Units.smallSpacing
    rightPadding: Kirigami.Units.smallSpacing
    bottomPadding: Kirigami.Units.smallSpacing

    Accessible.description: i18nc("@info:usagetip", "Open a menu")

    background: Rectangle {
        anchors.fill: parent
        radius: controlRoot.showHoverBackground ? controlRoot.hoverCornerRadius : 0
        color: {
            if (controlRoot.menuState === MenuDelegate.State.Down) {
                return Kirigami.Theme.highlightColor
            }
            if (controlRoot.menuState === MenuDelegate.State.Hover) {
                return Kirigami.ColorUtils.linearInterpolation(
                    Kirigami.Theme.backgroundColor,
                    Kirigami.Theme.hoverColor,
                    0.35)
            }
            return "transparent"
        }
    }

    contentItem: Item {
        implicitWidth: controlRoot.iconName.length > 0 ? contentIcon.implicitWidth : contentLabel.implicitWidth
        implicitHeight: controlRoot.iconName.length > 0 ? contentIcon.implicitHeight : contentLabel.implicitHeight

        Kirigami.Icon {
            id: contentIcon
            visible: controlRoot.iconName.length > 0
            source: controlRoot.iconName
            color: controlRoot.effectiveTextColor
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            anchors.centerIn: parent
        }

        PC3.Label {
            id: contentLabel
            visible: controlRoot.iconName.length === 0
            width: parent.width
            anchors.centerIn: parent
            text: controlRoot.Kirigami.MnemonicData.richTextLabel
            textFormat: Text.StyledText
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            font.family: controlRoot.fontFamily || Kirigami.Theme.defaultFont.family
            font.pointSize: controlRoot.fontSize > 0 ? controlRoot.fontSize : Kirigami.Theme.defaultFont.pointSize
            font.weight: controlRoot.fontWeight > 0 ? controlRoot.fontWeight : Kirigami.Theme.defaultFont.weight
            color: controlRoot.effectiveTextColor
        }
    }

    PC3.ToolTip.visible: hovered && controlRoot.iconName.length > 0
    PC3.ToolTip.text: controlRoot.text
}

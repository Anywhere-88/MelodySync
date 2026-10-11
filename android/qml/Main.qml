// ============================================================================
//  qml/Main.qml —— 骨架自检页
//  视觉对齐桌面版：纯黑底 + 深色玻璃面板 + 青绿强调色 #00e0b8。
//  正式 UI（阶段2）将直接复用桌面版 ui/index.html，本页仅作骨架验证。
// ============================================================================
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 360
    height: 720
    visible: true
    color: "#000000"
    title: qsTr("MelodySync")

    // ---- 探针结果落盘 ----
    property var rows: []
    property bool allOk: false

    Connections {
        target: backendProbe
        function onResultChanged() {
            root.rows = backendProbe.rows
            root.allOk = backendProbe.allOk
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // ---- 品牌头 ----
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 88
            Rectangle {
                anchors.fill: parent
                radius: 14
                color: "#141414"
                border.color: "#2a2a2a"
                border.width: 1
            }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 2
                Label {
                    text: "MelodySync"
                    font.pixelSize: 26
                    font.weight: Font.DemiBold
                    color: "#00e0b8"
                    Layout.leftMargin: 4
                }
                Label {
                    text: "Android 骨架 · 后端核心自检"
                    font.pixelSize: 12
                    color: "#8a8a8a"
                    Layout.leftMargin: 5
                }
            }
        }

        // ---- 自检结果列表 ----
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: root.rows
            delegate: Rectangle {
                width: list.width
                height: 52
                radius: 10
                color: "#111111"
                border.color: modelData.ok ? "#1e3d36" : "#4d1e26"
                border.width: 1
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: modelData.ok ? "#00e0b8" : "#ff4d6a"
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ColumnLayout {
                        spacing: 1
                        Layout.fillWidth: true
                        Label {
                            text: modelData.name
                            font.pixelSize: 12
                            color: "#8a8a8a"
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: modelData.value
                            font.pixelSize: 15
                            font.weight: Font.Medium
                            color: modelData.ok ? "#eaeaea" : "#ff4d6a"
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        // ---- 总状态 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            radius: 10
            color: root.allOk ? "#0d2420" : "#2a0f14"
            border.color: root.allOk ? "#00e0b8" : "#ff4d6a"
            border.width: 1
            Label {
                anchors.centerIn: parent
                text: root.rows.length === 0 ? "自检中…" :
                      (root.allOk ? "✓ 后端核心在安卓上运行正常" : "✗ 有自检项未通过")
                font.pixelSize: 14
                font.weight: Font.Medium
                color: root.allOk ? "#00e0b8" : "#ff4d6a"
            }
        }
    }

    Component.onCompleted: {
        if (typeof backendProbe !== "undefined")
            backendProbe.run()
    }
}

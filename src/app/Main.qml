import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import VolumeCommander

ApplicationWindow {
    id: win
    visible: true
    width: 1400; height: 900
    title: "volume-commander"

    AppState { id: app }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // 2x2 viewer grid
        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 2; rowSpacing: 2; columnSpacing: 2

            Repeater {
                model: ["xy", "xz", "yz", "seg"]
                ViewerItem {
                    required property string modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    state: app
                    view: modelData
                    Text {
                        text: parent.view; color: "#80ffffff"
                        x: 6; y: 4; font.pixelSize: 13
                    }
                }
            }
        }

        // control panel
        ColumnLayout {
            Layout.preferredWidth: 260
            Layout.fillHeight: true
            spacing: 8
            Layout.margins: 10

            Label { text: "volume-commander"; font.bold: true; font.pixelSize: 16 }

            Button {
                text: "Open volume…"
                Layout.fillWidth: true
                onClicked: app.openVolume(volUrl.text)
            }
            TextField { id: volUrl; Layout.fillWidth: true
                placeholderText: "s3://philodemos/forrest/…/volume.zarr" }

            Button {
                text: "Load segment…"
                Layout.fillWidth: true
                onClicked: app.loadSegment(segDir.text)
            }
            TextField { id: segDir; Layout.fillWidth: true; placeholderText: "/path/to/tifxyz" }

            MenuSeparator { Layout.fillWidth: true }

            Label { text: "Window: " + app.windowLow.toFixed(0) + " – " + app.windowHigh.toFixed(0) }
            Slider { Layout.fillWidth: true; from: 0; to: 255; value: app.windowLow
                     onMoved: app.windowLow = value }
            Slider { Layout.fillWidth: true; from: 0; to: 255; value: app.windowHigh
                     onMoved: app.windowHigh = value }

            CheckBox { text: "Composite"; checked: app.compositeEnabled
                       onToggled: app.compositeEnabled = checked }
            Label { text: "Layers front: " + app.layersFront }
            Slider { Layout.fillWidth: true; from: 0; to: 64; stepSize: 1; value: app.layersFront
                     onMoved: app.layersFront = value }
            Label { text: "Layers behind: " + app.layersBehind }
            Slider { Layout.fillWidth: true; from: 0; to: 64; stepSize: 1; value: app.layersBehind
                     onMoved: app.layersBehind = value }

            ComboBox {
                Layout.fillWidth: true
                model: ["mean", "max", "min", "alpha"]
                onActivated: app.compositeMethod = currentText
            }

            CheckBox { text: "Raking light"; checked: app.rakingEnabled
                       onToggled: app.rakingEnabled = checked }
            CheckBox { text: "CLAHE"; checked: app.claheEnabled
                       onToggled: app.claheEnabled = checked }

            MenuSeparator { Layout.fillWidth: true }
            CheckBox { text: "Paint 3D mask (L=paint, R=erase)"; checked: app.maskPaint
                       onToggled: app.maskPaint = checked }
            Label { text: "Brush radius: " + app.brushRadius.toFixed(0) }
            Slider { Layout.fillWidth: true; from: 1; to: 32; stepSize: 1; value: app.brushRadius
                     onMoved: app.brushRadius = value }
            Button { text: "Save mask…"; Layout.fillWidth: true
                     onClicked: app.saveMask(segDir.text + "_mask") }

            Item { Layout.fillHeight: true }
        }
    }
}

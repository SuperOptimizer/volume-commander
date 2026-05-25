import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import VolumeCommander

ApplicationWindow {
    id: win
    visible: true
    width: 1500; height: 950
    title: "volume-commander"
    color: "#1a1a1a"

    AppState { id: app }

    // Persist the volume/segment URLs across runs and auto-open them on launch.
    Settings {
        id: settings
        property string volumeUrl: ""
        property string segmentUrl: ""
    }
    Component.onCompleted: {
        if (settings.volumeUrl) { volUrl.text = settings.volumeUrl; app.openVolume(settings.volumeUrl) }
        if (settings.segmentUrl) { segUrl.text = settings.segmentUrl; app.loadSegment(settings.segmentUrl) }
    }

    // left: fixed 2x2 viewer grid | right: control panel
    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            property real cw: (width  - 2) / 2
            property real ch: (height - 2) / 2

            ViewerItem { x: 0;            y: 0;            width: grid.cw; height: grid.ch
                         state: app; view: "xy"; ViewerLabel { text: "XY" } }
            ViewerItem { x: grid.cw + 2;  y: 0;            width: grid.cw; height: grid.ch
                         state: app; view: "xz"; ViewerLabel { text: "XZ" } }
            ViewerItem { x: 0;            y: grid.ch + 2;  width: grid.cw; height: grid.ch
                         state: app; view: "yz"; ViewerLabel { text: "YZ" } }
            ViewerItem { x: grid.cw + 2;  y: grid.ch + 2;  width: grid.cw; height: grid.ch
                         state: app; view: "seg"; ViewerLabel { text: "SEGMENT" } }
        }

        Rectangle {
            Layout.preferredWidth: 280
            Layout.fillHeight: true
            color: "#262626"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Label { text: "volume-commander"; color: "white"; font.bold: true; font.pixelSize: 16 }

                Label { text: "Volume (S3)"; color: "#bbb" }
                TextField { id: volUrl; Layout.fillWidth: true
                    placeholderText: "s3://bucket/path/volume.zarr" }
                Button { text: "Open volume"; Layout.fillWidth: true
                    onClicked: { settings.volumeUrl = volUrl.text; app.openVolume(volUrl.text) } }

                Label { text: "Segment (S3)"; color: "#bbb" }
                TextField { id: segUrl; Layout.fillWidth: true
                    placeholderText: "s3://bucket/path/segment (tifxyz)" }
                Button { text: "Load segment"; Layout.fillWidth: true
                    onClicked: { settings.segmentUrl = segUrl.text; app.loadSegment(segUrl.text) } }

                MenuSeparator { Layout.fillWidth: true }

                Label { text: "Window: " + app.windowLow.toFixed(0) + " – " + app.windowHigh.toFixed(0); color: "#bbb" }
                Slider { Layout.fillWidth: true; from: 0; to: 255; value: app.windowLow;  onMoved: app.windowLow = value }
                Slider { Layout.fillWidth: true; from: 0; to: 255; value: app.windowHigh; onMoved: app.windowHigh = value }

                CheckBox { text: "Composite"; checked: app.compositeEnabled; onToggled: app.compositeEnabled = checked }
                Label { text: "Layers front: " + app.layersFront; color: "#bbb" }
                Slider { Layout.fillWidth: true; from: 0; to: 64; stepSize: 1; value: app.layersFront;  onMoved: app.layersFront = value }
                Label { text: "Layers behind: " + app.layersBehind; color: "#bbb" }
                Slider { Layout.fillWidth: true; from: 0; to: 64; stepSize: 1; value: app.layersBehind; onMoved: app.layersBehind = value }
                ComboBox { Layout.fillWidth: true; model: ["mean", "max", "min", "alpha"]
                    onActivated: app.compositeMethod = currentText }

                CheckBox { text: "Raking light"; checked: app.rakingEnabled; onToggled: app.rakingEnabled = checked }
                CheckBox { text: "CLAHE"; checked: app.claheEnabled; onToggled: app.claheEnabled = checked }

                MenuSeparator { Layout.fillWidth: true }
                CheckBox { text: "Paint 3D mask (L paint / R erase)"; checked: app.maskPaint; onToggled: app.maskPaint = checked }
                Label { text: "Brush radius: " + app.brushRadius.toFixed(0); color: "#bbb" }
                Slider { Layout.fillWidth: true; from: 1; to: 32; stepSize: 1; value: app.brushRadius; onMoved: app.brushRadius = value }
                Button { text: "Save mask"; Layout.fillWidth: true; onClicked: app.saveMask(volUrl.text) }

                Item { Layout.fillHeight: true }
                Label { text: "scroll = slice · drag = pan · wheel = zoom"; color: "#777"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }
    }

    component ViewerLabel: Text {
        anchors.left: parent.left; anchors.top: parent.top
        anchors.margins: 6; color: "#aae0ff"; font.pixelSize: 13; font.bold: true
    }
}

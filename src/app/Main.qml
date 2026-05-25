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
            property string maximized: ""   // "" = 2x2; else the maximized view name

            // One pane. Double-click (or the corner button) toggles maximize:
            // the chosen pane fills the whole panel; the others hide.
            component Pane: ViewerItem {
                id: pane
                required property string vname
                required property real gx
                required property real gy
                required property string vlabel
                state: app
                view: vname
                visible: grid.maximized === "" || grid.maximized === vname
                x: grid.maximized === vname ? 0 : gx
                y: grid.maximized === vname ? 0 : gy
                width:  grid.maximized === vname ? grid.width  : grid.cw
                height: grid.maximized === vname ? grid.height : grid.ch

                ViewerLabel { text: pane.vlabel }
                // maximize / restore toggle (top-right)
                Rectangle {
                    width: 22; height: 22; radius: 3; color: "#60000000"
                    anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 14
                           text: grid.maximized === pane.vname ? "❐" : "⛶" }
                    MouseArea { anchors.fill: parent; onClicked:
                        grid.maximized = (grid.maximized === pane.vname ? "" : pane.vname) }
                }
            }

            Pane { vname: "xy";  vlabel: "XY";      gx: 0;           gy: 0 }
            Pane { vname: "xz";  vlabel: "XZ";      gx: grid.cw + 2; gy: 0 }
            Pane { vname: "yz";  vlabel: "YZ";      gx: 0;           gy: grid.ch + 2 }
            Pane { vname: "seg"; vlabel: "SEGMENT"; gx: grid.cw + 2; gy: grid.ch + 2 }
        }

        Rectangle {
            id: panel
            property bool open: true
            Layout.preferredWidth: open ? 280 : 28
            Layout.fillHeight: true
            color: "#262626"
            Behavior on Layout.preferredWidth { NumberAnimation { duration: 120 } }

            // Collapse / expand toggle, always visible at the panel's top-left.
            Rectangle {
                width: 24; height: 24; radius: 3; color: "#3a3a3a"; z: 2
                anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 4
                Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 14
                       text: panel.open ? "⟩" : "⟨" }
                MouseArea { anchors.fill: parent; onClicked: panel.open = !panel.open }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                anchors.topMargin: 34
                spacing: 8
                visible: panel.open

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

                Label { text: "Interpolation"; color: "#bbb" }
                ComboBox { Layout.fillWidth: true
                    model: ["nearest", "trilinear"]
                    currentIndex: Math.max(0, model.indexOf(app.interpolation))
                    onActivated: app.interpolation = currentText }

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

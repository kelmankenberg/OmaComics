import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "scrapeDialog"
    property var paths: []
    property int position: 0
    property bool settingsOnly: false
    property string applyError: ""
    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property string comicPath: position < paths.length ? paths[position] : ""
    readonly property var proposed: {
        const language = translator.language
        const metadata = backend.scraper.metadata
        return backend.missingMetadata(comicPath)
    }

    function begin(selected) {
        paths = selected.slice()
        position = 0
        settingsOnly = false
        prepare()
        open()
    }

    function configure() {
        paths = []
        settingsOnly = true
        backend.scraper.cancel()
        open()
    }

    function prepare() {
        backend.scraper.cancel()
        applyError = ""
        query.text = backend.scrapeQuery(comicPath)
    }

    function advance() {
        if (position + 1 >= paths.length) {
            close()
        } else {
            position++
            prepare()
        }
    }

    anchors.centerIn: Overlay.overlay
    width: Math.min(parent.width - 32, 720)
    height: Math.min(parent.height - 32, 680)
    modal: true
    focus: true
    title: qsTr("Comic Vine metadata")
    standardButtons: Dialog.Close
    onClosed: {
        backend.scraper.cancel()
        apiKey.clear()
        applyError = ""
    }
    background: Rectangle {
        color: theme.background
        border.color: theme.colors.selection || theme.foreground
        radius: theme.radius
    }
    contentItem: ColumnLayout {
        spacing: 8
        LayoutMirroring.enabled: dialog.rightToLeft
        LayoutMirroring.childrenInherit: true
        Label {
            Layout.fillWidth: true
            text: qsTr("Search sends the query to Comic Vine. Review each match before filling missing fields. Original comic files are never modified.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: !backend.scraper.configured || dialog.settingsOnly
            text: qsTr("Use your own API key for personal, non-commercial use. Secure key storage can be configured in Settings.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !backend.scraper.configured || dialog.settingsOnly
            TextField {
                id: apiKey
                objectName: "comicVineApiKey"
                Layout.fillWidth: true
                placeholderText: qsTr("Comic Vine API key")
                echoMode: TextInput.Password
                maximumLength: 128
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                onAccepted: saveKey.clicked()
            }
            Button {
                id: saveKey
                objectName: "setComicVineKey"
                text: qsTr("Use key")
                enabled: !backend.scraper.busy && !backend.credentials.busy
                Material.roundedScale: theme.radius
                onClicked: {
                    backend.setComicVineKey(apiKey.text)
                    apiKey.clear()
                }
            }
        }
        Button {
            visible: !backend.scraper.configured || dialog.settingsOnly
            text: qsTr("Get an API key from Comic Vine")
            Material.roundedScale: theme.radius
            onClicked: Qt.openUrlExternally("https://comicvine.gamespot.com/api/")
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.settingsOnly && backend.scraper.configured
            text: qsTr("API key configured.")
            textFormat: Text.PlainText
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0 && (dialog.settingsOnly || !backend.scraper.configured)
            text: backend.credentials.message
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            visible: !dialog.settingsOnly
            text: qsTr("Comic %1 of %2: %3")
                .arg(Number(dialog.position + 1).toLocaleString(Qt.locale(), 'f', 0))
                .arg(Number(dialog.paths.length).toLocaleString(Qt.locale(), 'f', 0))
                .arg(backend.scrapeQuery(dialog.comicPath))
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !dialog.settingsOnly
            TextField {
                id: query
                objectName: "comicVineQuery"
                Layout.fillWidth: true
                placeholderText: qsTr("Series and issue number")
                maximumLength: 200
                enabled: !backend.scraper.busy
                onAccepted: if (search.enabled) search.clicked()
            }
            Button {
                id: search
                objectName: "searchComicVine"
                text: qsTr("Search")
                enabled: backend.scraper.configured && !backend.scraper.busy && query.text.trim().length > 0
                Material.roundedScale: theme.radius
                onClicked: {
                    dialog.applyError = ""
                    backend.scraper.search(query.text)
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.applyError || backend.scraper.message
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        ListView {
            id: results
            objectName: "comicVineMatches"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !dialog.settingsOnly && Object.keys(backend.scraper.metadata).length === 0
            clip: true
            model: backend.scraper.matches
            ScrollBar.vertical: ScrollBar { }
            delegate: ItemDelegate {
                id: match
                objectName: "comicVineMatch"
                required property var modelData
                width: results.width
                implicitHeight: Math.max(56, matchLabel.implicitHeight + 20)
                enabled: !backend.scraper.busy
                contentItem: Label {
                    id: matchLabel
                    text: qsTr("%1 #%2 — %3 (%4)").arg(match.modelData.series)
                        .arg(match.modelData.number).arg(match.modelData.title).arg(match.modelData.date)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                onClicked: backend.scraper.choose(modelData.id)
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !dialog.settingsOnly && !backend.scraper.busy
                && Object.keys(backend.scraper.metadata).length > 0
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Missing fields to fill")
                    textFormat: Text.PlainText
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    visible: dialog.proposed.length === 0
                    text: qsTr("No missing fields. Existing metadata will be kept.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: dialog.proposed
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        text: qsTr("%1: %2").arg(modelData.label).arg(modelData.value)
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !dialog.settingsOnly
            Button {
                objectName: "applyComicVine"
                text: qsTr("Apply and next")
                enabled: !backend.scraper.busy && Object.keys(backend.scraper.metadata).length > 0
                Material.roundedScale: theme.radius
                onClicked: {
                    if (backend.applyScrapedMetadata(dialog.comicPath))
                        dialog.advance()
                    else
                        dialog.applyError = backend.status
                }
            }
            Button {
                objectName: "skipComicVine"
                text: qsTr("Skip")
                Material.roundedScale: theme.radius
                onClicked: dialog.advance()
            }
        }
        Button {
            text: qsTr("Metadata provided by Comic Vine")
            Material.roundedScale: theme.radius
            onClicked: Qt.openUrlExternally(backend.scraper.metadata.sourceUrl || "https://comicvine.gamespot.com/")
        }
        Item { Layout.fillHeight: true; visible: dialog.settingsOnly }
    }
}

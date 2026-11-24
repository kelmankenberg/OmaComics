import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

FocusScope {
    id: pane
    objectName: "settingsPane"
    required property bool rightToLeft
    property int section: 0
    signal closed()
    signal configureComicVine()
    signal clearCacheRequested()
    onSectionChanged: {
        const flick = scroll.contentItem as Flickable
        if (flick) flick.contentY = 0
        if (section === 4) backend.refreshCacheSize()
    }
    onVisibleChanged: if (visible && section === 4) backend.refreshCacheSize()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            ToolButton {
                objectName: "closeSettingsButton"
                text: pane.rightToLeft ? "→" : "←"
                Accessible.name: qsTr("Back to library")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Back to library")
                onClicked: pane.closed()
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Settings")
                font.pixelSize: 24
                font.bold: true
                elide: Text.ElideRight
            }
        }
        ComboBox {
            objectName: "settingsCategory"
            Layout.fillWidth: true
            model: [qsTr("Startup"), qsTr("Reading"), qsTr("Library"), qsTr("Backups"),
                    qsTr("Image cache"), qsTr("Comic Vine"), qsTr("Feedback")]
            currentIndex: pane.section
            onActivated: pane.section = currentIndex
            Accessible.name: qsTr("Settings category")
            Material.roundedScale: theme.radius
        }
        ScrollView {
            id: scroll
            objectName: "settingsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: pane.section === 0
                    spacing: 8
                    Label { text: qsTr("When OmaComics opens"); font.bold: true }
                    ComboBox {
                        objectName: "startupBehavior"
                        Layout.fillWidth: true
                        model: [qsTr("Open the library"), qsTr("Preview the last comic"), qsTr("Resume reading automatically")]
                        currentIndex: backend.preferences.startupBehavior
                        onActivated: backend.setPreference("startupBehavior", currentIndex)
                        Accessible.name: qsTr("When OmaComics opens")
                        Material.roundedScale: theme.radius
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Opening a comic file or using --resume overrides this choice.")
                        wrapMode: Text.Wrap
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: pane.section === 1
                    spacing: 8
                    Label { text: qsTr("Fit mode"); font.bold: true }
                    ComboBox {
                        objectName: "settingsFitMode"
                        Layout.fillWidth: true
                        model: [qsTr("Fit page"), qsTr("Fit width"), qsTr("Original size")]
                        currentIndex: backend.preferences.fitMode
                        onActivated: backend.setPreference("fitMode", currentIndex)
                        Accessible.name: qsTr("Fit mode")
                        Material.roundedScale: theme.radius
                    }
                    Label {
                        Layout.fillWidth: true
                        text: {
                            const language = translator.language
                            return qsTr("Zoom: %1%").arg(Number(100 + backend.preferences.zoomStep * 25).toLocaleString(Qt.locale(), 'f', 0))
                        }
                    }
                    Slider {
                        objectName: "settingsZoom"
                        Layout.fillWidth: true
                        from: 0; to: 8; stepSize: 1
                        value: backend.preferences.zoomStep
                        onMoved: backend.setPreference("zoomStep", Math.round(value))
                        Accessible.name: qsTr("Zoom")
                    }
                    CheckBox {
                        objectName: "settingsDoublePage"
                        text: qsTr("Two-page")
                        checked: backend.doublePage
                        onToggled: backend.setDoublePage(checked)
                    }
                    Label { text: qsTr("Page navigation options"); font.bold: true }
                    CheckBox {
                        objectName: "settingsArrowNavigation"
                        text: qsTr("Arrow keys")
                        checked: backend.arrowNavigation
                        onToggled: backend.setArrowNavigation(checked)
                    }
                    CheckBox {
                        objectName: "settingsWheelNavigation"
                        text: qsTr("Mouse wheel")
                        checked: backend.wheelNavigation
                        onToggled: backend.setWheelNavigation(checked)
                    }
                    CheckBox {
                        objectName: "settingsMouseNavigation"
                        text: qsTr("Mouse buttons")
                        checked: backend.mouseNavigation
                        onToggled: backend.setMouseNavigation(checked)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Right-click advances; left-click goes back. The wheel pans enlarged pages before turning.")
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Manga direction is remembered per comic, not globally. Use the reader switch, or select comics or folders and use the selection menu to change several at once.")
                        wrapMode: Text.Wrap
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: pane.section === 2
                    spacing: 8
                    CheckBox {
                        objectName: "scanOnStartup"
                        text: qsTr("Check library at startup")
                        checked: backend.preferences.scanOnStartup
                        onToggled: backend.setPreference("scanOnStartup", checked)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Only new or changed comics are indexed. Scanning runs in the background while you read. Turn this off to scan manually.")
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "settingsScanButton"
                        Layout.fillWidth: true
                        text: qsTr("Rescan library")
                        enabled: !backend.scanning && !backend.cacheBusy
                        Material.roundedScale: theme.radius
                        onClicked: backend.scanLibrary()
                    }
                    Label { text: qsTr("Library view"); font.bold: true }
                    ComboBox {
                        objectName: "settingsLibraryView"
                        Layout.fillWidth: true
                        model: [qsTr("List view"), qsTr("Folder tree")]
                        currentIndex: backend.treeView ? 1 : 0
                        onActivated: backend.setTreeView(currentIndex === 1)
                        Accessible.name: qsTr("Library view")
                        Material.roundedScale: theme.radius
                    }
                    CheckBox {
                        objectName: "settingsLibraryVisible"
                        text: qsTr("Show library panel")
                        checked: backend.preferences.libraryVisible
                        onToggled: backend.setPreference("libraryVisible", checked)
                    }
                    Label { text: qsTr("Cover thumbnail size"); font.bold: true }
                    Slider {
                        objectName: "settingsThumbnailSize"
                        Layout.fillWidth: true
                        from: 40; to: 192; stepSize: 8
                        value: backend.thumbnailSize === 0 ? 40 : backend.thumbnailSize
                        onMoved: backend.setThumbnailSize(value < 48 ? 0 : Math.round(value))
                        Accessible.name: qsTr("Cover thumbnail size")
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("The leftmost position hides thumbnails.")
                        wrapMode: Text.Wrap
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: pane.section === 3
                    spacing: 8
                    Label { text: qsTr("Automatic backup retention"); font.bold: true }
                    ComboBox {
                        objectName: "backupRetention"
                        Layout.fillWidth: true
                        model: [qsTr("7 days"), qsTr("30 days"), qsTr("90 days"), qsTr("Keep forever")]
                        currentIndex: [7, 30, 90, 0].indexOf(backend.preferences.backupRetentionDays)
                        onActivated: backend.setPreference("backupRetentionDays", [7, 30, 90, 0][currentIndex])
                        Accessible.name: qsTr("Automatic backup retention")
                        Material.roundedScale: theme.radius
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Expired daily and new safety backups are removed automatically. Manual and older unclassified backups are never removed.")
                        wrapMode: Text.Wrap
                    }
                    Label { text: qsTr("Backup location"); font.bold: true }
                    Label {
                        objectName: "backupLocationLabel"
                        Layout.fillWidth: true
                        text: backend.backupDirectory
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Automatic and manual backups use this folder. Existing backups stay in their original location.")
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "changeBackupLocationButton"
                        Layout.fillWidth: true
                        text: qsTr("Change folder…")
                        Material.roundedScale: theme.radius
                        onClicked: folderPicker.openBackupFolder(backend.backupDirectory)
                    }
                    Button {
                        objectName: "defaultBackupLocationButton"
                        Layout.fillWidth: true
                        text: qsTr("Use default location")
                        Material.roundedScale: theme.radius
                        onClicked: backend.setBackupDirectory("")
                    }
                    Button {
                        objectName: "backupNowButton"
                        Layout.fillWidth: true
                        text: qsTr("Back up now")
                        Material.roundedScale: theme.radius
                        onClicked: backend.backupLibrary()
                    }
                    Button {
                        objectName: "restoreBackupButton"
                        Layout.fillWidth: true
                        text: qsTr("Restore library…")
                        enabled: !backend.scanning && !backend.reading
                        Material.roundedScale: theme.radius
                        onClicked: folderPicker.openRestoreFile(backend.backupDirectory)
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: pane.section === 4
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: {
                            const language = translator.language
                            return qsTr("Cached images: %1 MiB").arg(Number(backend.cacheBytes / 1048576).toLocaleString(Qt.locale(), 'f', 1))
                        }
                        wrapMode: Text.Wrap
                    }
                    BusyIndicator { running: backend.cacheBusy; visible: running }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Only generated covers and page images are cleared. Page indexes, comic files, metadata, bookmarks, and reading progress are kept. Close the reader first.")
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "refreshCacheButton"
                        Layout.fillWidth: true
                        text: qsTr("Refresh cache size")
                        enabled: !backend.cacheBusy
                        Material.roundedScale: theme.radius
                        onClicked: backend.refreshCacheSize()
                    }
                    Button {
                        objectName: "clearCacheButton"
                        Layout.fillWidth: true
                        text: qsTr("Clear cached images…")
                        enabled: !backend.cacheBusy && !backend.reading && !backend.scanning
                        Material.roundedScale: theme.radius
                        onClicked: pane.clearCacheRequested()
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: pane.section === 5
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: backend.scraper.configured ? qsTr("API key configured.") : qsTr("No API key configured.")
                        wrapMode: Text.Wrap
                    }
                    CheckBox {
                        objectName: "rememberApiKey"
                        text: qsTr("Remember API key securely")
                        enabled: !backend.credentials.busy
                        checked: backend.preferences.rememberApiKey
                        onToggled: backend.setPreference("rememberApiKey", checked)
                    }
                    Label {
                        Layout.fillWidth: true
                        text: backend.credentials.available
                            ? qsTr("Keys use the desktop credential store, never application files or backups. Re-enter the key to save it after enabling this option.")
                            : qsTr("Desktop credential storage is unavailable. The API key remains session-only.")
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "configureComicVineButton"
                        Layout.fillWidth: true
                        text: qsTr("Comic Vine API key…")
                        enabled: !backend.credentials.busy
                        Material.roundedScale: theme.radius
                        onClicked: pane.configureComicVine()
                    }
                    Button {
                        objectName: "forgetComicVineKeyButton"
                        Layout.fillWidth: true
                        text: qsTr("Forget API key")
                        enabled: !backend.credentials.busy
                        Material.roundedScale: theme.radius
                        onClicked: backend.forgetComicVineKey()
                    }
                    Label {
                        Layout.fillWidth: true
                        text: backend.credentials.message
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        visible: text.length > 0
                    }
                    CheckBox {
                        id: seriesLookup
                        objectName: "useSeriesForLookup"
                        text: qsTr("Search by series and issue number")
                        Layout.fillWidth: true
                        checked: backend.preferences.useSeriesForLookup
                        onToggled: backend.setPreference("useSeriesForLookup", checked)
                        contentItem: Label {
                            text: seriesLookup.text
                            wrapMode: Text.Wrap
                            leftPadding: seriesLookup.mirrored ? 0 : seriesLookup.indicator.width + seriesLookup.spacing
                            rightPadding: seriesLookup.mirrored ? seriesLookup.indicator.width + seriesLookup.spacing : 0
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("When disabled, searches start with the comic title. Lookups remain manual, reviewed, and fill missing metadata only.")
                        wrapMode: Text.Wrap
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: pane.section === 6
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Opens a GitHub form in your browser. Review it before submitting; a GitHub account is required.")
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "reportBugButton"
                        Layout.fillWidth: true
                        text: qsTr("Report a bug")
                        Material.roundedScale: theme.radius
                        onClicked: backend.openFeedback(true)
                    }
                    Button {
                        objectName: "requestFeatureButton"
                        Layout.fillWidth: true
                        text: qsTr("Request a feature")
                        Material.roundedScale: theme.radius
                        onClicked: backend.openFeedback(false)
                    }
                }
            }
        }
    }
}

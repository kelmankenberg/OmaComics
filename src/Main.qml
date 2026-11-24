import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1160
    height: 760
    minimumWidth: 820
    minimumHeight: 560
    visible: true
    title: qsTr("OmaComics")
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    property string selectedPath: ""
    readonly property bool libraryVisible: backend.preferences.libraryVisible
    property bool settingsOpen: false
    onLibraryVisibleChanged: if (!libraryVisible) settingsOpen = false
    property string libraryFilter: ""
    readonly property int zoomStep: backend.preferences.zoomStep
    readonly property int fitMode: backend.preferences.fitMode
    readonly property real zoomFactor: 1 + zoomStep * 0.25
    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property bool typing: searchField.activeFocus || collectionField.activeFocus || scrapeDialog.visible || settingsOpen
    readonly property var selectedBook: {
        const books = backend.comics
        for (let i = 0; i < books.length; ++i)
            if (books[i].path === selectedPath)
                return books[i]
        return null
    }

    function selectBook(path) {
        selectedPath = path
        collectionField.text = selectedBook ? selectedBook.collection || "" : ""
        backend.ensureCover(path)
    }

    function toggleSettings() {
        backend.setPreference("libraryVisible", true)
        settingsOpen = !settingsOpen
        if (settingsOpen)
            settingsPane.forceActiveFocus()
        else
            settingsButton.forceActiveFocus()
    }

    function selectLibraryFilter(filter) {
        libraryFilter = filter
        backend.setCollectionFilter(filter)
    }

    function turnPage(forward, fromWheel) {
        const previous = backend.currentPage
        if (forward) backend.nextPage()
        else backend.previousPage()
        if (backend.currentPage !== previous) {
            pageViewport.wheelDistance = 0
            pageViewport.pendingBottom = fromWheel && !forward
            pageViewport.contentY = pageViewport.pendingBottom
                ? Math.max(0, pageViewport.contentHeight - pageViewport.height) : 0
        }
    }

    Shortcut { sequence: "?"; enabled: !win.typing; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequence: "Q"; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "Return"; enabled: !win.typing && !!win.selectedBook && !backend.reading; onActivated: backend.openComic(win.selectedPath) }
    Shortcut {
        sequence: "Escape"
        enabled: (backend.reading || win.settingsOpen) && !scrapeDialog.visible && !restoreDialog.visible && !removeFolderDialog.visible && !cacheDialog.visible
        onActivated: {
            if (win.settingsOpen) win.toggleSettings()
            else backend.closeReader()
        }
    }
    Shortcut { sequence: "Right"; enabled: backend.reading && backend.arrowNavigation && !win.typing; onActivated: win.turnPage(!backend.rightToLeft, false) }
    Shortcut { sequence: "Left"; enabled: backend.reading && backend.arrowNavigation && !win.typing; onActivated: win.turnPage(backend.rightToLeft, false) }
    Shortcut { sequence: "Space"; enabled: backend.reading && !win.typing; onActivated: win.turnPage(true, false) }
    Shortcut { sequence: "Backspace"; enabled: backend.reading && !win.typing; onActivated: win.turnPage(false, false) }
    Shortcut { sequence: "B"; enabled: backend.reading && !win.typing; onActivated: backend.toggleBookmark() }
    Shortcut { sequence: "Ctrl+B"; onActivated: backend.setPreference("libraryVisible", !win.libraryVisible) }
    Shortcut { sequence: "Ctrl+,"; onActivated: win.toggleSettings() }

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Rectangle {
                objectName: "libraryPanel"
                Layout.preferredWidth: 340
                Layout.fillHeight: true
                visible: win.libraryVisible
                color: theme.colors.lighter_background || theme.background

                ColumnLayout {
                    objectName: "libraryNavigation"
                    visible: !win.settingsOpen
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: win.height < 650 ? 3 : 10

                    RowLayout {
                        Layout.fillWidth: true
                        ToolButton {
                            objectName: "hideLibraryButton"
                            text: "☰"
                            font.pixelSize: 20
                            Accessible.name: qsTr("Hide library panel")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Hide library panel")
                            onClicked: backend.setPreference("libraryVisible", false)
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("OmaComics")
                            font.pixelSize: 24
                            font.bold: true
                        }
                        ToolButton {
                            objectName: "scanButton"
                            text: "↻"
                            Accessible.name: qsTr("Rescan library")
                            onClicked: backend.scanLibrary()
                        }
                        ToolButton {
                            id: settingsButton
                            objectName: "settingsButton"
                            text: "⚙"
                            font.pixelSize: 20
                            Accessible.name: qsTr("Settings")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Settings")
                            onClicked: win.toggleSettings()
                        }
                    }

                    TextField {
                        id: searchField
                        objectName: "searchField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("Search comics")
                        maximumLength: 256
                        onTextChanged: backend.setSearchText(text)
                        onAccepted: {
                            backend.commitSearchTerm()
                            clear()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Press Enter to add a search term")
                        font.pixelSize: 12
                        color: theme.foreground
                    }

                    Flickable {
                        id: searchChips
                        objectName: "searchChips"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(chipFlow.implicitHeight,
                            Math.max(32, Math.min(104, 64 + (win.height - 560) * 0.4)
                                - (win.height < 650 && backend.collections.length > 0 ? 40 : 0)))
                        visible: backend.searchTerms.length > 0
                        clip: true
                        contentWidth: width
                        contentHeight: chipFlow.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {
                            id: chipScrollBar
                            objectName: "searchChipScrollBar"
                            policy: searchChips.contentHeight > searchChips.height
                                ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                        }
                        Flow {
                            id: chipFlow
                            width: searchChips.width - chipScrollBar.width
                            x: win.rightToLeft ? chipScrollBar.width : 0
                            spacing: 6
                            Repeater {
                                model: backend.searchTerms
                                delegate: Rectangle {
                                    id: chip
                                    required property string modelData
                                    required property int index
                                    objectName: "searchChip"
                                    width: Math.min(chipFlow.width, chipLabel.implicitWidth + removeChip.implicitWidth + 18)
                                    height: 32
                                    radius: Math.min(4, theme.radius)
                                    color: theme.colors.selection || theme.background
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 2
                                        spacing: 0
                                        Label {
                                            id: chipLabel
                                            Layout.fillWidth: true
                                            text: chip.modelData
                                            textFormat: Text.PlainText
                                            elide: Text.ElideRight
                                        }
                                        ToolButton {
                                            id: removeChip
                                            objectName: "removeSearchChip"
                                            Layout.preferredWidth: 30
                                            Layout.preferredHeight: 30
                                            text: "×"
                                            Accessible.name: qsTr("Remove search term %1").arg(chip.modelData)
                                            onClicked: backend.removeSearchTerm(chip.index)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            id: allBooksButton
                            objectName: "allBooksButton"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: win.height < 650 ? 36 : 44
                            text: qsTr("All books")
                            highlighted: win.libraryFilter === ""
                            Accessible.role: Accessible.RadioButton
                            Accessible.checkable: true
                            Accessible.checked: highlighted
                            Material.roundedScale: theme.radius
                            contentItem: Label {
                                text: allBooksButton.text
                                color: allBooksButton.highlighted ? theme.accentForeground : theme.foreground
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            onClicked: win.selectLibraryFilter("")
                        }
                        Button {
                            id: recentBooksButton
                            objectName: "recentBooksButton"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: win.height < 650 ? 36 : 44
                            text: qsTr("Recently read")
                            highlighted: win.libraryFilter === "__recent__"
                            Accessible.role: Accessible.RadioButton
                            Accessible.checkable: true
                            Accessible.checked: highlighted
                            Material.roundedScale: theme.radius
                            contentItem: Label {
                                text: recentBooksButton.text
                                color: recentBooksButton.highlighted ? theme.accentForeground : theme.foreground
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                            onClicked: win.selectLibraryFilter("__recent__")
                        }
                    }

                    ComboBox {
                        id: collectionFilter
                        objectName: "collectionFilter"
                        Layout.fillWidth: true
                        Layout.preferredHeight: win.height < 650 ? 40 : 56
                        visible: backend.collections.length > 0
                        model: [qsTr("Collections")].concat(backend.collections)
                        currentIndex: Math.max(0, backend.collections.indexOf(win.libraryFilter) + 1)
                        contentItem: Label {
                            textFormat: Text.PlainText
                            text: collectionFilter.displayText
                            leftPadding: 16
                            rightPadding: 16
                            horizontalAlignment: Text.AlignLeft
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        delegate: ItemDelegate {
                            id: collectionItem
                            required property var modelData
                            width: collectionFilter.width
                            contentItem: Label {
                                textFormat: Text.PlainText
                                text: collectionItem.modelData
                                horizontalAlignment: Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                        }
                        onActivated: win.selectLibraryFilter(currentIndex === 0 ? "" : backend.collections[currentIndex - 1])
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            objectName: "addFolderButton"
                            Layout.fillWidth: true
                            text: qsTr("Add folder")
                            Material.roundedScale: theme.radius
                            onClicked: folderPicker.openFolder()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Cover thumbnail size")
                        color: theme.colors.muted || theme.foreground
                    }
                    Slider {
                        objectName: "thumbnailSize"
                        Layout.fillWidth: true
                        from: 40
                        to: 192
                        stepSize: 8
                        value: backend.thumbnailSize === 0 ? 40 : backend.thumbnailSize
                        onMoved: backend.setThumbnailSize(value < 48 ? 0 : Math.round(value))
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            objectName: "libraryFoldersLabel"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: qsTr("Library folders")
                            font.bold: true
                            elide: Text.ElideRight
                            ToolTip.visible: labelHover.hovered
                            ToolTip.text: qsTr("%Ln books", "", backend.comics.length)
                            HoverHandler { id: labelHover }
                        }
                        Rectangle {
                            objectName: "libraryActionGroup"
                            implicitWidth: libraryActions.implicitWidth + 2
                            implicitHeight: libraryActions.implicitHeight + 2
                            color: theme.background
                            border.color: theme.colors.selection || theme.foreground
                            radius: Math.min(4, theme.radius)
                            ButtonGroup { id: libraryViewGroup }
                            RowLayout {
                                id: libraryActions
                                anchors.centerIn: parent
                                spacing: 0
                                ToolButton {
                                    objectName: "listViewButton"
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 32
                                    icon.source: "qrc:/list-view.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    checkable: true
                                    checked: !backend.treeView
                                    ButtonGroup.group: libraryViewGroup
                                    Accessible.name: qsTr("List view")
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("List view")
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.setTreeView(false)
                                }
                                ToolButton {
                                    objectName: "treeViewButton"
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 32
                                    icon.source: "qrc:/folder-tree.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    checkable: true
                                    checked: backend.treeView
                                    ButtonGroup.group: libraryViewGroup
                                    Accessible.name: qsTr("Folder tree")
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Folder tree")
                                    Material.roundedScale: theme.radius
                                    onClicked: backend.setTreeView(true)
                                }
                                ToolButton {
                                    objectName: "scrapeSelectedButton"
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 32
                                    icon.source: "qrc:/scrape.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    enabled: backend.selectedComics.length > 0
                                    Accessible.name: qsTr("Scrape…")
                                    Accessible.description: qsTr("%Ln selected comics", "", backend.selectedComics.length)
                                    ToolTip.visible: hovered
                                    ToolTip.text: backend.selectedComics.length > 0
                                        ? qsTr("Scrape (%1)…").arg(Number(backend.selectedComics.length).toLocaleString(Qt.locale(), 'f', 0))
                                        : qsTr("Scrape…")
                                    Material.roundedScale: theme.radius
                                    onClicked: scrapeDialog.begin(backend.selectedComics)
                                }
                                ToolButton {
                                    objectName: "selectionMenuButton"
                                    Layout.preferredWidth: 28
                                    Layout.preferredHeight: 32
                                    text: "⋮"
                                    Accessible.name: qsTr("Comic selection options")
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Comic selection options")
                                    Material.roundedScale: theme.radius
                                    onClicked: selectionMenu.open()
                                    Menu {
                                        id: selectionMenu
                                        objectName: "selectionMenu"
                                        background: Rectangle {
                                            implicitWidth: Math.max(240, selectAllOption.implicitWidth,
                                                                   clearSelectionOption.implicitWidth, apiKeyOption.implicitWidth,
                                                                   selectedMangaOption.implicitWidth, selectedLeftToRightOption.implicitWidth)
                                            color: theme.background
                                            border.color: theme.colors.selection || theme.foreground
                                            radius: theme.radius
                                        }
                                        MenuItem {
                                            id: selectAllOption
                                            text: qsTr("Select all visible comics")
                                            onTriggered: backend.selectAllVisible()
                                        }
                                        MenuItem {
                                            id: clearSelectionOption
                                            text: qsTr("Clear selection")
                                            onTriggered: backend.clearSelection()
                                        }
                                        MenuItem {
                                            id: apiKeyOption
                                            text: qsTr("Comic Vine API key…")
                                            onTriggered: scrapeDialog.configure()
                                        }
                                        MenuSeparator { }
                                        MenuItem {
                                            id: selectedMangaOption
                                            objectName: "selectedMangaOption"
                                            text: qsTr("Manga direction for selected")
                                            enabled: backend.selectedComics.length > 0
                                            onTriggered: backend.setSelectedManga(true)
                                        }
                                        MenuItem {
                                            id: selectedLeftToRightOption
                                            objectName: "selectedLeftToRightOption"
                                            text: qsTr("Left-to-right for selected")
                                            enabled: backend.selectedComics.length > 0
                                            onTriggered: backend.setSelectedManga(false)
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ListView {
                        id: folders
                        objectName: "folderList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, win.height < 650 ? 24 : 72)
                        visible: count > 0
                        clip: true
                        model: backend.libraryFolders
                        ScrollBar.vertical: ScrollBar {
                            id: folderScrollBar
                            objectName: "folderScrollBar"
                            policy: folders.contentHeight > folders.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                        }
                        delegate: Item {
                            id: folderRow
                            required property string modelData
                            width: folders.width
                            height: 24
                            Label {
                                anchors.left: parent.left
                                anchors.right: removeFolder.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: folderRow.modelData
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                font.pixelSize: 11
                                color: theme.colors.muted || theme.foreground
                            }
                            ToolButton {
                                id: removeFolder
                                objectName: "removeLibraryFolderButton"
                                anchors.right: parent.right
                                anchors.rightMargin: folderScrollBar.policy === ScrollBar.AlwaysOn ? folderScrollBar.width : 0
                                anchors.verticalCenter: parent.verticalCenter
                                width: 28
                                height: 24
                                text: "×"
                                Accessible.name: qsTr("Remove library folder")
                                enabled: !backend.scanning
                                onClicked: {
                                    removeFolderDialog.folderPath = folderRow.modelData
                                    removeFolderDialog.open()
                                }
                            }
                        }
                    }

                    ListView {
                        id: bookList
                        objectName: "bookList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: !backend.treeView
                        clip: true
                        spacing: 6
                        model: backend.visibleComicModel
                        ScrollBar.vertical: ScrollBar {
                            id: bookScrollBar
                            objectName: "bookScrollBar"
                            policy: bookList.contentHeight > bookList.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                        }
                        delegate: Item {
                            id: row
                            objectName: "comicRow"
                            required property var comic
                            required property int index
                            width: bookList.width
                            height: backend.thumbnailSize === 0 ? 56 : Math.max(72, backend.thumbnailSize + 12)

                            Rectangle {
                                anchors.fill: parent
                                anchors.rightMargin: bookScrollBar.policy === ScrollBar.AlwaysOn ? bookScrollBar.width : 0
                                radius: Math.min(4, theme.radius)
                                color: row.comic.path === win.selectedPath
                                    ? theme.colors.selection || theme.accent
                                    : "transparent"
                            }
                            Image {
                                id: cover
                                objectName: "bookCover"
                                anchors.left: selectBookCheck.right
                                anchors.leftMargin: backend.thumbnailSize === 0 ? 0 : 6
                                anchors.verticalCenter: parent.verticalCenter
                                width: backend.thumbnailSize
                                height: backend.thumbnailSize
                                visible: backend.thumbnailSize > 0
                                source: visible ? row.comic.cover : ""
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: true
                                Component.onCompleted: {
                                    if (visible) backend.ensureCover(row.comic.path)
                                }
                            }
                            Rectangle {
                                anchors.fill: cover
                                visible: cover.visible && cover.status !== Image.Ready
                                color: theme.colors.selection || theme.background
                                radius: Math.min(4, theme.radius)
                                z: -1
                            }
                            ColumnLayout {
                                anchors.left: cover.right
                                anchors.right: recentRemove.visible ? recentRemove.left : parent.right
                                anchors.leftMargin: 10
                                anchors.rightMargin: 8 + (bookScrollBar.policy === ScrollBar.AlwaysOn ? bookScrollBar.width : 0)
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3
                                Label {
                                    Layout.fillWidth: true
                                    text: row.comic.title
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    font.bold: true
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: row.comic.series || row.comic.format
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    font.pixelSize: 12
                                    color: theme.colors.muted || theme.foreground
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: Number(row.comic.progress) > 0
                                    text: qsTr("Page %1 of %2")
                                        .arg(Number(row.comic.progress).toLocaleString(Qt.locale()))
                                        .arg(Number(row.comic.pageCount).toLocaleString(Qt.locale()))
                                    color: theme.colors.muted || theme.foreground
                                    font.pixelSize: 11
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                anchors.rightMargin: bookScrollBar.policy === ScrollBar.AlwaysOn ? bookScrollBar.width : 0
                                preventStealing: true
                                onClicked: win.selectBook(row.comic.path)
                                onDoubleClicked: {
                                    win.selectBook(row.comic.path)
                                    backend.openComic(row.comic.path)
                                }
                            }
                            CheckBox {
                                id: selectBookCheck
                                objectName: "selectComicCheck"
                                z: 1
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 32
                                checked: backend.selectedComics.indexOf(row.comic.path) >= 0
                                Accessible.name: qsTr("Select %1").arg(row.comic.title)
                                onToggled: backend.selectComic(row.comic.path, checked)
                            }
                            ToolButton {
                                id: recentRemove
                                objectName: "removeRecentBook"
                                z: 1
                                anchors.right: parent.right
                                anchors.rightMargin: bookScrollBar.policy === ScrollBar.AlwaysOn ? bookScrollBar.width : 0
                                anchors.verticalCenter: parent.verticalCenter
                                width: 30
                                height: 30
                                visible: win.libraryFilter === "__recent__"
                                text: "×"
                                Accessible.name: qsTr("Remove %1 from recently read").arg(row.comic.title)
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Remove from recently read")
                                onClicked: backend.removeFromRecent(row.comic.path)
                            }
                            Connections {
                                target: backend
                                function onComicsChanged() {
                                    if (backend.thumbnailSize > 0)
                                        backend.ensureCover(row.comic.path)
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: bookList.count === 0
                            text: backend.libraryFolders.length === 0
                                ? qsTr("Add a folder to build your library")
                                : qsTr("No matching comics")
                            color: theme.colors.muted || theme.foreground
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                    LibraryTree {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: backend.treeView
                        selectedPath: win.selectedPath
                        onPreview: function(path) { win.selectBook(path) }
                        onRead: function(path) {
                            win.selectBook(path)
                            backend.openComic(path)
                        }
                    }
                }
                SettingsPane {
                    id: settingsPane
                    anchors.fill: parent
                    visible: win.settingsOpen
                    rightToLeft: win.rightToLeft
                    onClosed: win.toggleSettings()
                    onConfigureComicVine: scrapeDialog.configure()
                    onClearCacheRequested: cacheDialog.open()
                }
            }

            Rectangle {
                objectName: "mainArea"
                Layout.minimumWidth: 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: theme.background

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        ToolButton {
                            objectName: "showLibraryButton"
                            visible: !win.libraryVisible
                            text: "☰"
                            font.pixelSize: 20
                            Accessible.name: qsTr("Show library panel")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Show library panel")
                            onClicked: backend.setPreference("libraryVisible", true)
                        }
                        Label {
                            objectName: "status"
                            Layout.fillWidth: true
                            text: backend.status
                            textFormat: Text.PlainText
                            color: theme.colors.muted || theme.foreground
                            elide: Text.ElideRight
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: !backend.reading

                        RowLayout {
                            anchors.centerIn: parent
                            width: Math.min(parent.width, 900)
                            height: Math.min(parent.height, 650)
                            spacing: 24
                            visible: !!win.selectedBook

                            Rectangle {
                                Layout.preferredWidth: Math.min(340, parent.width * 0.43)
                                Layout.fillHeight: true
                                color: theme.colors.lighter_background || theme.background

                                Image {
                                    id: selectedCover
                                    objectName: "selectedCover"
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    source: win.selectedBook ? win.selectedBook.cover : ""
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    cache: true
                                }
                            }
                            Flickable {
                                id: detailsScroller
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                contentWidth: width
                                contentHeight: Math.max(height, detailsColumn.implicitHeight)
                                boundsBehavior: Flickable.StopAtBounds
                                ScrollBar.vertical: ScrollBar {
                                    id: detailsScrollBar
                                    policy: detailsScroller.contentHeight > detailsScroller.height
                                        ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                                }

                                ColumnLayout {
                                    id: detailsColumn
                                    width: detailsScroller.width - detailsScrollBar.width
                                    x: win.rightToLeft ? detailsScrollBar.width : 0
                                    y: Math.max(0, (detailsScroller.height - implicitHeight) / 2)
                                    spacing: 14

                                    Label {
                                        Layout.fillWidth: true
                                        text: win.selectedBook ? win.selectedBook.title : ""
                                        textFormat: Text.PlainText
                                        font.pixelSize: 30
                                        font.bold: true
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: win.selectedBook
                                            ? [win.selectedBook.series, win.selectedBook.number
                                                ? qsTr("Issue %1").arg(win.selectedBook.number) : "",
                                               win.selectedBook.year].filter(Boolean).join(" · ")
                                            : ""
                                        textFormat: Text.PlainText
                                        color: theme.colors.muted || theme.foreground
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: win.selectedBook
                                            ? [win.selectedBook.writer, win.selectedBook.publisher, win.selectedBook.genre]
                                                .filter(Boolean).join(" · ")
                                            : ""
                                        textFormat: Text.PlainText
                                        color: theme.colors.muted || theme.foreground
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: win.selectedBook ? win.selectedBook.summary || "" : ""
                                        textFormat: Text.PlainText
                                        color: theme.colors.muted || theme.foreground
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 8
                                        elide: Text.ElideRight
                                    }
                                    Button {
                                        visible: !!win.selectedBook && !!win.selectedBook.sourceUrl
                                        text: qsTr("Metadata provided by Comic Vine")
                                        Material.roundedScale: theme.radius
                                        onClicked: Qt.openUrlExternally(win.selectedBook.sourceUrl)
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField {
                                            id: collectionField
                                            objectName: "collectionField"
                                            Layout.fillWidth: true
                                            placeholderText: qsTr("Collection")
                                            enabled: !!win.selectedBook
                                        }
                                        Button {
                                            text: qsTr("Save")
                                            enabled: !!win.selectedBook
                                            Material.roundedScale: theme.radius
                                            onClicked: backend.setCollection(win.selectedPath, collectionField.text)
                                        }
                                    }
                                    Button {
                                        objectName: "readButton"
                                        Layout.alignment: Qt.AlignHCenter
                                        text: qsTr("Read / Resume")
                                        highlighted: true
                                        enabled: !!win.selectedBook
                                        Material.roundedScale: theme.radius
                                        onClicked: backend.openComic(win.selectedPath)
                                        contentItem: Label {
                                            text: qsTr("Read / Resume")
                                            color: theme.accentForeground
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: !win.selectedBook
                            text: qsTr("Choose a comic from your library")
                            color: theme.colors.muted || theme.foreground
                        }
                    }

                    ColumnLayout {
                        Layout.minimumWidth: 0
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: backend.reading
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: backend.currentTitle
                                textFormat: Text.PlainText
                                font.pixelSize: 20
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("%1 / %2")
                                    .arg(Number(backend.currentPage).toLocaleString(Qt.locale()))
                                    .arg(Number(backend.pageCount).toLocaleString(Qt.locale()))
                                color: theme.colors.muted || theme.foreground
                            }
                            ToolButton {
                                objectName: "bookmarkButton"
                                text: backend.currentBookmarked ? "★" : "☆"
                                Accessible.name: backend.currentBookmarked ? qsTr("Remove bookmark") : qsTr("Add bookmark")
                                onClicked: backend.toggleBookmark()
                            }
                            ComboBox {
                                objectName: "bookmarks"
                                Layout.preferredWidth: 130
                                model: backend.bookmarks
                                displayText: count > 0 ? qsTr("Bookmarks") : qsTr("No bookmarks")
                                onActivated: backend.openPage(Number(currentText))
                            }
                            ToolButton {
                                objectName: "navigationMenuButton"
                                text: "⋮"
                                Accessible.name: qsTr("Page navigation options")
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Page navigation options")
                                onClicked: navigationMenu.open()
                                Menu {
                                    id: navigationMenu
                                    objectName: "navigationMenu"
                                    background: Rectangle {
                                        implicitWidth: Math.max(200, arrowOption.implicitWidth,
                                                                wheelOption.implicitWidth, mouseOption.implicitWidth)
                                        color: theme.background
                                        border.color: theme.colors.selection || theme.foreground
                                        radius: theme.radius
                                    }
                                    MenuItem {
                                        id: arrowOption
                                        objectName: "arrowNavigationOption"
                                        text: qsTr("Arrow keys")
                                        checkable: true
                                        checked: backend.arrowNavigation
                                        onTriggered: backend.setArrowNavigation(!backend.arrowNavigation)
                                    }
                                    MenuItem {
                                        id: wheelOption
                                        objectName: "wheelNavigationOption"
                                        text: qsTr("Mouse wheel")
                                        checkable: true
                                        checked: backend.wheelNavigation
                                        onTriggered: backend.setWheelNavigation(!backend.wheelNavigation)
                                    }
                                    MenuItem {
                                        id: mouseOption
                                        objectName: "mouseNavigationOption"
                                        text: qsTr("Mouse buttons")
                                        checkable: true
                                        checked: backend.mouseNavigation
                                        onTriggered: backend.setMouseNavigation(!backend.mouseNavigation)
                                    }
                                }
                            }
                            ToolButton {
                                objectName: "closeReaderButton"
                                text: "×"
                                Accessible.name: qsTr("Close reader")
                                onClicked: backend.closeReader()
                            }
                        }

                        Flickable {
                            id: pageViewport
                            objectName: "pageViewport"
                            property real wheelDistance: 0
                            property bool pendingBottom: false
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            contentWidth: Math.max(width, pagesRow.implicitWidth)
                            contentHeight: Math.max(height, pagesRow.implicitHeight)
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.horizontal: ScrollBar { }
                            ScrollBar.vertical: ScrollBar { objectName: "readerScrollBar" }
                            onContentHeightChanged: {
                                if (pendingBottom)
                                    contentY = Math.max(0, contentHeight - height)
                            }
                            onMovementStarted: pendingBottom = false

                            RowLayout {
                                id: pagesRow
                                layoutDirection: backend.rightToLeft ? Qt.RightToLeft : Qt.LeftToRight
                                width: Math.max(pageViewport.width, implicitWidth)
                                height: Math.max(pageViewport.height, implicitHeight)
                                spacing: backend.doublePage ? 8 : 0

                                Image {
                                    objectName: "comicPage"
                                    Layout.preferredWidth: win.fitMode === 2 && sourceSize.width > 0
                                        ? sourceSize.width * win.zoomFactor
                                        : Math.max(120, (pageViewport.width - pagesRow.spacing)
                                            / (backend.doublePage && backend.secondPagePath ? 2 : 1)
                                            * win.zoomFactor)
                                    Layout.preferredHeight: sourceSize.width <= 0
                                        ? pageViewport.height * win.zoomFactor
                                        : win.fitMode === 1
                                            ? Layout.preferredWidth * sourceSize.height / sourceSize.width
                                            : win.fitMode === 2
                                                ? sourceSize.height * win.zoomFactor
                                                : pageViewport.height * win.zoomFactor
                                    source: backend.pagePath
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    onStatusChanged: {
                                        if (status === Image.Ready || status === Image.Error) {
                                            Qt.callLater(function() {
                                                if (pageViewport.pendingBottom) {
                                                    pageViewport.contentY = Math.max(0, pageViewport.contentHeight - pageViewport.height)
                                                    pageViewport.pendingBottom = false
                                                }
                                            })
                                        }
                                    }
                                }
                                Image {
                                    objectName: "comicSecondPage"
                                    visible: backend.doublePage && backend.secondPagePath.length > 0
                                    Layout.preferredWidth: win.fitMode === 2 && sourceSize.width > 0
                                        ? sourceSize.width * win.zoomFactor
                                        : Math.max(120, (pageViewport.width - pagesRow.spacing) / 2 * win.zoomFactor)
                                    Layout.preferredHeight: sourceSize.width <= 0
                                        ? pageViewport.height * win.zoomFactor
                                        : win.fitMode === 1
                                            ? Layout.preferredWidth * sourceSize.height / sourceSize.width
                                            : win.fitMode === 2
                                                ? sourceSize.height * win.zoomFactor
                                                : pageViewport.height * win.zoomFactor
                                    source: backend.secondPagePath
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                }
                            }
                            MouseArea {
                                anchors.fill: pagesRow
                                acceptedButtons: Qt.NoButton
                                TapHandler {
                                    acceptedButtons: Qt.LeftButton
                                    enabled: backend.mouseNavigation
                                    onTapped: win.turnPage(false, false)
                                }
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    enabled: backend.mouseNavigation
                                    onTapped: win.turnPage(true, false)
                                }
                                onWheel: function(wheel) {
                                    if (wheel.modifiers !== Qt.NoModifier) {
                                        wheel.accepted = false
                                        return
                                    }
                                    const delta = wheel.pixelDelta.y !== 0
                                        ? wheel.pixelDelta.y : wheel.angleDelta.y * 0.8
                                    if (delta === 0) {
                                        wheel.accepted = false
                                        return
                                    }
                                    pageViewport.pendingBottom = false
                                    const limit = Math.max(0, pageViewport.contentHeight - pageViewport.height)
                                    const position = Math.max(0, Math.min(limit, pageViewport.contentY))
                                    if ((delta < 0 && position < limit - 1) || (delta > 0 && position > 1)) {
                                        pageViewport.contentY = Math.max(0, Math.min(limit, position - delta))
                                        pageViewport.wheelDistance = 0
                                        return
                                    }
                                    if (!backend.wheelNavigation) {
                                        wheel.accepted = false
                                        return
                                    }
                                    const step = wheel.angleDelta.y !== 0
                                        ? wheel.angleDelta.y : wheel.pixelDelta.y
                                    if (pageViewport.wheelDistance * step < 0)
                                        pageViewport.wheelDistance = 0
                                    pageViewport.wheelDistance += step
                                    if (Math.abs(pageViewport.wheelDistance) >= 120) {
                                        pageViewport.wheelDistance = 0
                                        win.turnPage(step < 0, true)
                                    }
                                }
                            }
                        }

                        Flow {
                            objectName: "readerControls"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 5
                            ToolButton {
                                text: backend.rightToLeft ? "→" : "←"
                                Accessible.name: qsTr("Previous page")
                                enabled: backend.currentPage > 1
                                onClicked: win.turnPage(false, false)
                            }
                            ToolButton {
                                text: backend.rightToLeft ? "←" : "→"
                                Accessible.name: qsTr("Next page")
                                enabled: backend.currentPage < backend.pageCount
                                    && (!backend.doublePage || backend.currentPage === 1
                                        || backend.currentPage + 2 <= backend.pageCount)
                                onClicked: win.turnPage(true, false)
                            }
                            ComboBox {
                                id: fitChooser
                                width: 150
                                model: [qsTr("Fit page"), qsTr("Fit width"), qsTr("Original size")]
                                currentIndex: win.fitMode
                                onActivated: backend.setPreference("fitMode", currentIndex)
                            }
                            ToolButton {
                                text: "−"
                                Accessible.name: qsTr("Zoom out")
                                enabled: win.zoomStep > 0
                                onClicked: backend.setPreference("zoomStep", win.zoomStep - 1)
                            }
                            Slider {
                                width: 130
                                from: 0
                                to: 8
                                stepSize: 1
                                value: win.zoomStep
                                onMoved: backend.setPreference("zoomStep", Math.round(value))
                            }
                            ToolButton {
                                text: "+"
                                Accessible.name: qsTr("Zoom in")
                                enabled: win.zoomStep < 8
                                onClicked: backend.setPreference("zoomStep", win.zoomStep + 1)
                            }
                            Switch {
                                text: qsTr("Two-page")
                                checked: backend.doublePage
                                onToggled: backend.setDoublePage(checked)
                            }
                            Switch {
                                text: qsTr("Manga")
                                checked: backend.rightToLeft
                                onToggled: backend.setRightToLeft(checked)
                            }
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: backend
        function onPreviewRequested(path) { win.selectBook(path) }
    }

    Connections {
        target: folderPicker
        function onBackupFolderSelected(path) {
            backend.setBackupDirectory(path)
        }
        function onRestoreFileSelected(path) {
            restoreDialog.backupPath = path
            restoreDialog.open()
        }
    }

    ScrapeDialog { id: scrapeDialog }

    Dialog {
        id: cacheDialog
        objectName: "clearCacheDialog"
        anchors.centerIn: Overlay.overlay
        width: Math.min(440, win.width - 40)
        modal: true
        focus: true
        title: qsTr("Clear cached images?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: backend.clearImageCache()
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection || theme.foreground
            radius: theme.radius
        }
        contentItem: Label {
            LayoutMirroring.enabled: win.rightToLeft
            text: qsTr("Covers and page images will be generated again when needed. Your comics, metadata, bookmarks, and reading progress will not be deleted.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }

    Dialog {
        id: removeFolderDialog
        objectName: "removeFolderDialog"
        property string folderPath: ""
        anchors.centerIn: Overlay.overlay
        width: Math.min(440, win.width - 40)
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        title: qsTr("Remove library folder?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: backend.removeLibraryFolder(folderPath)
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection || theme.foreground
            radius: theme.radius
        }
        contentItem: Label {
            LayoutMirroring.enabled: win.rightToLeft
            text: qsTr("Remove %1 and its books from the library? A safety backup will be saved. Your comic files will not be deleted.").arg(removeFolderDialog.folderPath)
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: theme.foreground
        }
    }

    Dialog {
        id: restoreDialog
        objectName: "restoreDialog"
        property string backupPath: ""
        anchors.centerIn: Overlay.overlay
        width: Math.min(440, win.width - 40)
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        title: qsTr("Restore library backup?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: backend.restoreLibrary(backupPath)
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection || theme.foreground
            radius: theme.radius
        }
        contentItem: Label {
            LayoutMirroring.enabled: win.rightToLeft
            text: qsTr("This replaces the current library and reading history. A safety backup of the current library will be saved first.")
            wrapMode: Text.Wrap
            color: theme.foreground
        }
    }

    Popup {
        id: help
        objectName: "help"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 20
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        ColumnLayout {
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true
            spacing: 8
            Label { text: qsTr("Keyboard shortcuts"); font.bold: true; font.pixelSize: 20 }
            Repeater {
                model: [
                    { keys: "Return", action: qsTr("Read the selected comic") },
                    { keys: "Left / Right", action: qsTr("Turn pages") },
                    { keys: "Space / Backspace", action: qsTr("Next / previous page") },
                    { keys: "B", action: qsTr("Toggle bookmark") },
                    { keys: "Ctrl+B", action: qsTr("Toggle library panel") },
                    { keys: "Ctrl+,", action: qsTr("Settings") },
                    { keys: "Escape", action: qsTr("Close the reader") },
                    { keys: "?", action: qsTr("Show or hide shortcuts") },
                    { keys: "Q", action: qsTr("Quit OmaComics") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    spacing: 20
                    Label { Layout.preferredWidth: 150; text: modelData.keys; color: theme.accent; font.bold: true }
                    Label { text: modelData.action }
                }
            }
        }
    }
}

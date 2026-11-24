import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "ReadingStatus.js" as ReadingStatus

BarWidget {
  id: root
  moduleName: "kel.omacomics"
  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  readonly property string dataDirectory: Quickshell.env("XDG_DATA_HOME")
    || (Quickshell.env("HOME") + "/.local/share")
  readonly property string statusPath: dataDirectory + "/omacomics/current.json"
  property var readingStatus: ReadingStatus.parse("")
  readonly property string currentTitle: readingStatus.title
  readonly property int currentPage: readingStatus.page
  readonly property int pageCount: readingStatus.pageCount
  readonly property int progress: readingStatus.progress
  readonly property bool inProgress: readingStatus.inProgress
  readonly property bool statusAvailable: readingStatus.available
  property bool popupOpen: false

  Commons.Translations {
    id: strings
    directory: Qt.resolvedUrl("i18n")
  }

  function updateStatus(raw, readFailed) {
    readingStatus = ReadingStatus.parse(raw, readFailed)
  }

  function close() {
    popupOpen = false
  }

  FileView {
    path: root.statusPath
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.updateStatus(text())
    onLoadFailed: function(error) {
      root.updateStatus("", error !== FileViewError.FileNotFound)
    }
  }

  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "󰂺"
    active: root.inProgress
    activeColor: Commons.Color.accent
    tooltipText: strings.t("tooltip")
    onPressed: function(mouseButton) {
      if (mouseButton === Qt.LeftButton) root.popupOpen = !root.popupOpen
      else if (mouseButton === Qt.RightButton) root.bar.run("omacomics")
    }
  }

  PopupCard {
    id: popup
    anchorItem: root
    bar: root.bar
    owner: root
    open: root.popupOpen
    contentWidth: popup.fittedContentWidth(Style.space(320))
    contentHeight: popup.fittedContentHeight(column.implicitHeight)

    Column {
      id: column
      width: parent.width
      spacing: Style.space(10)

      Text {
        textFormat: Text.PlainText
        text: strings.t("Current reading")
        color: Commons.Color.popups.text
        font.family: Style.font.family
        font.pixelSize: Style.font.subtitle
        font.bold: true
      }

      Text {
        textFormat: Text.PlainText
        width: parent.width
        text: !root.statusAvailable ? strings.t("Status unavailable")
          : root.currentTitle || strings.t("No comic in progress")
        color: Commons.Color.popups.text
        font.family: Style.font.family
        font.pixelSize: Style.font.body
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
      }

      Text {
        textFormat: Text.PlainText
        visible: root.pageCount > 0
        text: strings.t("Page %1 of %2")
          .arg(Commons.I18n.formatNumber(root.currentPage))
          .arg(Commons.I18n.formatNumber(root.pageCount))
        color: Commons.Color.popups.text
        font.family: Style.font.family
        font.pixelSize: Style.font.body
      }

      ProgressBar {
        width: parent.width
        visible: root.pageCount > 0
        from: 0
        to: 100
        value: root.progress
      }

      Row {
        spacing: Style.space(8)
        Button {
          text: strings.t("Open current comic")
          enabled: root.currentTitle.length > 0
          onClicked: {
            root.close()
            root.bar.run("omacomics --resume")
          }
        }
        Button {
          text: strings.t("Open library")
          onClicked: {
            root.close()
            root.bar.run("omacomics")
          }
        }
      }
    }
  }
}

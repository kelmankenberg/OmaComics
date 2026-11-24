import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

TreeView {
    id: tree
    objectName: "comicTree"
    property string selectedPath: ""
    signal preview(string path)
    signal read(string path)
    clip: true
    model: backend.comicTreeModel
    boundsBehavior: Flickable.StopAtBounds
    columnWidthProvider: function(column) { return width }
    ScrollBar.vertical: ScrollBar { }
    delegate: TreeViewDelegate {
        id: treeRow
        objectName: "comicTreeRow"
        required property bool folder
        required property string filePath
        required property var comic
        required property int selectionState
        implicitWidth: tree.width
        implicitHeight: 40
        indentation: 14
        leftPadding: mirrored ? 8 : leftMargin + depth * indentation + (indicator ? indicator.width + spacing : 0)
        rightPadding: mirrored ? leftMargin + depth * indentation + (indicator ? indicator.width + spacing : 0) : 8
        highlighted: !folder && filePath === tree.selectedPath
        background: Rectangle {
            color: treeRow.highlighted || treeRow.hovered ? theme.colors.selection || theme.background : "transparent"
            radius: Math.min(4, theme.radius)
        }
        contentItem: RowLayout {
            spacing: 4
            CheckBox {
                objectName: "treeComicCheck"
                Layout.preferredWidth: 32
                tristate: treeRow.folder
                checkState: treeRow.selectionState
                nextCheckState: function() {
                    return checkState === Qt.Checked ? Qt.Unchecked : Qt.Checked
                }
                Accessible.name: qsTr("Select %1").arg(treeRow.folder
                    ? treeRow.filePath.split("/").pop() || treeRow.filePath : treeRow.comic.title)
                onClicked: {
                    if (treeRow.folder)
                        backend.selectFolder(treeRow.filePath, checkState === Qt.Checked)
                    else
                        backend.selectComic(treeRow.filePath, checkState === Qt.Checked)
                }
            }
            Label {
                objectName: "treeComicLabel"
                Layout.fillWidth: true
                text: treeRow.folder ? treeRow.filePath.split("/").pop() || treeRow.filePath : treeRow.comic.title
                textFormat: Text.PlainText
                color: theme.foreground
                horizontalAlignment: Text.AlignLeft
                elide: Text.ElideRight
            }
        }
        onClicked: {
            if (folder)
                tree.toggleExpanded(row)
            else
                tree.preview(filePath)
        }
        onDoubleClicked: if (!folder) tree.read(filePath)
    }
    Label {
        anchors.centerIn: parent
        visible: backend.visibleComics.length === 0
        text: qsTr("No matching comics")
        textFormat: Text.PlainText
        color: theme.foreground
    }
}

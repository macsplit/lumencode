import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import org.kde.kirigami 2.19 as Kirigami
import Lumencode 1.0

Kirigami.ApplicationWindow {
    id: root

    title: "LumenCode"
    width: Kirigami.Units.gridUnit * 60
    height: Kirigami.Units.gridUnit * 34
    visible: true

    color: "#10141b"
    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.ToolBar

    ProjectController {
        id: project
    }

    property var fileModel: project.fileSystemModel
    property string draftRootPath: project.rootPath
    property bool projectLoaded: false

    function iconForFileType(fileType, isDir) {
        if (isDir) {
            return "folder";
        }
        if (fileType === "php") {
            return "code-context";
        }
        if (fileType === "html") {
            return "text-html";
        }
        if (fileType === "css") {
            return "format-text-code";
        }
        if (fileType === "react") {
            return "code-function";
        }
        if (fileType === "package") {
            return "package-x-generic";
        }
        if (fileType === "json") {
            return "application-json";
        }
        return "text-x-script";
    }

    function openProject(path) {
        if (!path || path === "") {
            return;
        }
        draftRootPath = path
        project.setRootPath(path)
        projectLoaded = true
        pageStack.clear()
        pageStack.push(explorerPageComponent)
    }

    function goToPicker() {
        projectLoaded = false
        pageStack.clear()
        pageStack.push(pickerPageComponent)
    }

    Component {
        id: pickerPageComponent

        Kirigami.Page {
            title: "Open Project"

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 24)
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    text: "LumenCode"
                    level: 1
                    horizontalAlignment: Text.AlignHCenter
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Kirigami.Theme.disabledTextColor
                    text: "Choose a project folder to open the structural explorer."
                }

                TextField {
                    Layout.fillWidth: true
                    text: root.draftRootPath
                    placeholderText: "/path/to/project"
                    onTextChanged: root.draftRootPath = text
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: Kirigami.Units.largeSpacing

                    Button {
                        text: "Browse Folder"
                        onClicked: {
                            var path = project.pickFolder()
                            if (path && path.length > 0) {
                                root.openProject(path)
                            }
                        }
                    }

                    Button {
                        text: "Open"
                        enabled: root.draftRootPath.length > 0
                        onClicked: root.openProject(root.draftRootPath)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Kirigami.Theme.disabledTextColor
                    text: "This build targets the current Qt5/KF5 Kirigami stack and uses bundled parser sources inside the repository."
                }
            }
        }
    }

    Component {
        id: explorerPageComponent

        Kirigami.Page {
            title: "Explorer"
            actions {
                main: Kirigami.Action {
                    text: "Back"
                    icon.name: "go-previous-symbolic"
                    onTriggered: root.goToPicker()
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing

                Kirigami.AbstractCard {
                    Layout.fillHeight: true
                    Layout.preferredWidth: root.width * 0.32

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Heading {
                            text: "Filesystem"
                            level: 2
                        }

                        Label {
                            text: "."
                            color: Kirigami.Theme.disabledTextColor
                            elide: Text.ElideMiddle
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: fileModel ? fileModel.visibleEntries : []

                            delegate: ItemDelegate {
                                required property var modelData
                                width: ListView.view.width
                                highlighted: modelData.path === project.selectedPath

                                contentItem: RowLayout {
                                    spacing: Kirigami.Units.smallSpacing

                                    Item {
                                        Layout.preferredWidth: modelData.depth * Kirigami.Units.largeSpacing
                                        Layout.fillHeight: true
                                    }

                                    ToolButton {
                                        visible: modelData.isDir && modelData.hasChildren
                                        text: modelData.expanded ? "-" : "+"
                                        onClicked: fileModel.toggleExpanded(modelData.path)
                                    }

                                    Kirigami.Icon {
                                        source: root.iconForFileType(modelData.fileType, modelData.isDir)
                                        implicitWidth: Kirigami.Units.iconSizes.smallMedium
                                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        elide: Text.ElideMiddle
                                    }
                                }

                                onClicked: {
                                    project.selectPath(modelData.path)
                                    if (modelData.isDir) {
                                        fileModel.toggleExpanded(modelData.path)
                                    }
                                }
                            }
                        }
                    }
                }

                Kirigami.AbstractCard {
                    Layout.fillHeight: true
                    Layout.preferredWidth: root.width * 0.34

                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Heading {
                            text: "Overview"
                            level: 2
                        }

                        Label {
                            text: project.selectedFileData.fileName || "No selection"
                            font.bold: true
                            elide: Text.ElideMiddle
                        }

                        Label {
                            text: project.selectedFileData.summary || "Select a file to inspect its structure."
                            color: Kirigami.Theme.disabledTextColor
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            visible: project.selectedFileData.path
                            text: project.selectedRelativePath || ""
                            color: Kirigami.Theme.disabledTextColor
                            elide: Text.ElideMiddle
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: project.selectedFileData.symbols || []

                            delegate: Kirigami.AbstractCard {
                                required property var modelData
                                width: ListView.view.width

                                contentItem: ColumnLayout {
                                    spacing: Kirigami.Units.smallSpacing

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: modelData.kind
                                            color: Kirigami.Theme.highlightColor
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            font.bold: true
                                            elide: Text.ElideMiddle
                                        }

                                        Label {
                                            text: "L" + modelData.line
                                            color: Kirigami.Theme.disabledTextColor
                                        }
                                    }

                                    Button {
                                        text: "Inspect"
                                        onClicked: project.selectSymbolByData(modelData)
                                    }
                                }
                            }
                        }
                    }
                }

                Kirigami.AbstractCard {
                    Layout.fillHeight: true
                    Layout.preferredWidth: root.width * 0.34

                    contentItem: ScrollView {
                        clip: true

                        ColumnLayout {
                            width: parent.width
                            spacing: Kirigami.Units.largeSpacing

                            Kirigami.Heading {
                                text: "Detail"
                                level: 2
                            }

                            Label {
                                visible: !!project.selectedSymbol.name
                                text: project.selectedSymbol.kind ? project.selectedSymbol.kind + ": " + project.selectedSymbol.name : ""
                                font.bold: true
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                visible: !!project.selectedSymbol.detail
                                text: project.selectedSymbol.detail || ""
                                color: Kirigami.Theme.disabledTextColor
                                wrapMode: Text.WordWrap
                            }

                            Repeater {
                                model: project.selectedSymbolMembers || []

                                delegate: RowLayout {
                                    required property var modelData
                                    width: parent.width

                                    Label {
                                        text: modelData.kind
                                        color: Kirigami.Theme.highlightColor
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        elide: Text.ElideMiddle
                                    }

                                    Label {
                                        text: "L" + modelData.line
                                        color: Kirigami.Theme.disabledTextColor
                                    }
                                }
                            }

                            Kirigami.Separator {
                                visible: (project.selectedFileData.dependencies || []).length > 0
                                Layout.fillWidth: true
                            }

                            Kirigami.Heading {
                                visible: (project.selectedFileData.dependencies || []).length > 0
                                text: "Dependencies"
                                level: 3
                            }

                            Repeater {
                                model: project.selectedFileData.dependencies || []

                                delegate: Button {
                                    required property var modelData
                                    width: parent.width
                                    text: modelData.type + ": " + modelData.label + (modelData.line ? " (L" + modelData.line + ")" : "")
                                    enabled: !!modelData.path && modelData.exists
                                    onClicked: {
                                        if (modelData.path && modelData.exists) {
                                            project.selectPath(modelData.path)
                                        }
                                    }
                                }
                            }

                            Kirigami.Separator {
                                visible: (project.selectedFileData.routes || []).length > 0
                                Layout.fillWidth: true
                            }

                            Kirigami.Heading {
                                visible: (project.selectedFileData.routes || []).length > 0
                                text: "Routes"
                                level: 3
                            }

                            Repeater {
                                model: project.selectedFileData.routes || []

                                delegate: RowLayout {
                                    required property var modelData
                                    width: parent.width

                                    Label {
                                        text: modelData.method
                                        color: Kirigami.Theme.highlightColor
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.path
                                        elide: Text.ElideMiddle
                                    }

                                    Label {
                                        text: "L" + modelData.line
                                        color: Kirigami.Theme.disabledTextColor
                                    }
                                }
                            }

                            Kirigami.Separator {
                                visible: (project.selectedFileData.relatedFiles || []).length > 0
                                Layout.fillWidth: true
                            }

                            Kirigami.Heading {
                                visible: (project.selectedFileData.relatedFiles || []).length > 0
                                text: "Related Files"
                                level: 3
                            }

                            Repeater {
                                model: project.selectedFileData.relatedFiles || []

                                delegate: Button {
                                    required property var modelData
                                    width: parent.width
                                    text: modelData.type + ": " + modelData.label
                                    onClicked: {
                                        if (modelData.path && modelData.exists) {
                                            project.selectPath(modelData.path)
                                        }
                                    }
                                }
                            }

                            Kirigami.Separator {
                                visible: !!project.selectedFileData.packageSummary
                                         && (!!project.selectedFileData.packageSummary.name
                                             || (project.selectedFileData.packageSummary.dependencies || []).length > 0)
                                Layout.fillWidth: true
                            }

                            Kirigami.Heading {
                                visible: !!project.selectedFileData.packageSummary
                                         && (!!project.selectedFileData.packageSummary.name
                                             || (project.selectedFileData.packageSummary.dependencies || []).length > 0)
                                text: "Package"
                                level: 3
                            }

                            Label {
                                visible: !!project.selectedFileData.packageSummary && !!project.selectedFileData.packageSummary.name
                                text: (project.selectedFileData.packageSummary.name || "")
                                      + (project.selectedFileData.packageSummary.version ? "@" + project.selectedFileData.packageSummary.version : "")
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                visible: !!project.selectedFileData.packageSummary && !!project.selectedFileData.packageSummary.main
                                text: "Main: " + ((project.selectedFileData.packageSummary && project.selectedFileData.packageSummary.main)
                                                  ? project.selectedFileData.packageSummary.main : "")
                                Layout.fillWidth: true
                                color: Kirigami.Theme.disabledTextColor
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                visible: !!project.selectedFileData.packageSummary
                                         && (project.selectedFileData.packageSummary.dependencies || []).length > 0
                                text: "Dependencies: "
                                      + ((project.selectedFileData.packageSummary && project.selectedFileData.packageSummary.dependencies)
                                         ? project.selectedFileData.packageSummary.dependencies.length : 0)
                                color: Kirigami.Theme.disabledTextColor
                            }

                            Repeater {
                                model: project.selectedFileData.packageSummary && project.selectedFileData.packageSummary.scripts
                                       ? Object.keys(project.selectedFileData.packageSummary.scripts)
                                       : []

                                delegate: ColumnLayout {
                                    required property string modelData
                                    width: parent.width

                                    Label {
                                        text: "script: " + modelData
                                        color: Kirigami.Theme.highlightColor
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: (project.selectedFileData.packageSummary
                                               && project.selectedFileData.packageSummary.scripts
                                               && project.selectedFileData.packageSummary.scripts[modelData])
                                              ? project.selectedFileData.packageSummary.scripts[modelData]
                                              : ""
                                        wrapMode: Text.WordWrap
                                        color: Kirigami.Theme.disabledTextColor
                                    }
                                }
                            }

                            Kirigami.Separator {
                                visible: (project.selectedFileData.quickLinks || []).length > 0
                                Layout.fillWidth: true
                            }

                            Kirigami.Heading {
                                visible: (project.selectedFileData.quickLinks || []).length > 0
                                text: "Quick Links"
                                level: 3
                            }

                            Repeater {
                                model: project.selectedFileData.quickLinks || []

                                delegate: Button {
                                    required property var modelData
                                    width: parent.width
                                    text: modelData.type + ": " + modelData.label + (modelData.exists ? "" : " (missing)")
                                    onClicked: {
                                        if (modelData.exists) {
                                            project.selectPath(modelData.path)
                                        }
                                    }
                                }
                            }

                            Kirigami.Separator {
                                visible: !!project.selectedFileData.cssSummary
                                           && ((project.selectedFileData.cssSummary.usedClasses || []).length > 0
                                               || (project.selectedFileData.cssSummary.availableClasses || []).length > 0)
                                Layout.fillWidth: true
                            }

                            Kirigami.Heading {
                                visible: !!project.selectedFileData.cssSummary
                                         && (project.selectedFileData.cssSummary.usedClasses || []).length > 0
                                text: "CSS Classes"
                                level: 3
                            }

                            Label {
                                visible: !!project.selectedFileData.cssSummary
                                         && (project.selectedFileData.cssSummary.matchedClasses || []).length > 0
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: "Matched:\n" + (project.selectedFileData.cssSummary.matchedClasses || []).join(", ")
                            }

                            Label {
                                visible: !!project.selectedFileData.cssSummary
                                         && (project.selectedFileData.cssSummary.missingClasses || []).length > 0
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: "#f7a072"
                                text: "Missing:\n" + (project.selectedFileData.cssSummary.missingClasses || []).join(", ")
                            }
                        }
                    }
                }
            }
        }
    }

    Component.onCompleted: {
        draftRootPath = project.lastOpenedPath() || project.rootPath
        if (project.restoreLastOpenedPath()) {
            projectLoaded = true
            pageStack.clear()
            pageStack.push(explorerPageComponent)
        } else {
            goToPicker()
        }
    }
}

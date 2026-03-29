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
    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.None

    ProjectController {
        id: project
    }

    property var fileModel: project.fileSystemModel
    property string draftRootPath: project.rootPath
    property bool projectLoaded: false
    property int compactMargin: Math.max(2, Math.round(Kirigami.Units.largeSpacing * 0.35))
    property int compactSpacing: Math.max(4, Math.round(Kirigami.Units.smallSpacing * 0.7))
    property int compactRowSpacing: Math.max(3, Math.round(Kirigami.Units.smallSpacing * 0.5))
    property int compactFontSize: Math.max(9, Kirigami.Theme.defaultFont.pointSize - 1)
    property int compactSmallFontSize: Math.max(8, compactFontSize - 1)

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

    function diagnosticColor(severity) {
        if (severity === "warning") {
            return "#f7a072";
        }
        if (severity === "error") {
            return "#bf616a";
        }
        return Kirigami.Theme.disabledTextColor;
    }

    function snippetRelativePath(path) {
        if (!path) {
            return "";
        }
        if (!project.rootPath || path.indexOf(project.rootPath) !== 0) {
            return path;
        }

        var relative = path.slice(project.rootPath.length);
        if (!relative || relative.length === 0) {
            return "/";
        }
        if (relative.charAt(0) !== "/") {
            relative = "/" + relative;
        }
        return relative;
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
                spacing: root.compactSpacing

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
                    font.pointSize: root.compactFontSize
                    text: "Choose a project folder to open the structural explorer."
                }

                TextField {
                    Layout.fillWidth: true
                    font.pointSize: root.compactFontSize
                    text: root.draftRootPath
                    placeholderText: "/path/to/project"
                    onTextChanged: root.draftRootPath = text
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: root.compactSpacing

                    Button {
                        text: "Browse Folder"
                        font.pointSize: root.compactFontSize
                        onClicked: {
                            var path = project.pickFolder()
                            if (path && path.length > 0) {
                                root.openProject(path)
                            }
                        }
                    }

                    Button {
                        text: "Open"
                        font.pointSize: root.compactFontSize
                        enabled: root.draftRootPath.length > 0
                        onClicked: root.openProject(root.draftRootPath)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Kirigami.Theme.disabledTextColor
                    font.pointSize: root.compactSmallFontSize
                    text: "This build targets the current Qt5/KF5 Kirigami stack and uses bundled parser sources inside the repository."
                }
            }
        }
    }

    Component {
        id: explorerPageComponent

        Kirigami.Page {
            title: "Explorer"

            SplitView {
                anchors.fill: parent
                anchors.margins: root.compactMargin
                orientation: Qt.Vertical

                SplitView {
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    SplitView.preferredHeight: root.height * 0.66
                    orientation: Qt.Horizontal

                    RowLayout {
                        SplitView.fillHeight: true
                        SplitView.preferredWidth: root.width * 0.32
                        SplitView.minimumWidth: Kirigami.Units.gridUnit * 12
                        spacing: root.compactRowSpacing

                        Item {
                            Layout.fillHeight: true
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 2.4

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: root.compactRowSpacing

                                ToolButton {
                                    Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
                                    icon.name: "go-previous-symbolic"
                                    display: AbstractButton.IconOnly
                                    font.pointSize: root.compactSmallFontSize
                                    onClicked: root.goToPicker()
                                }

                                Item {
                                    Layout.fillHeight: true
                                }
                            }
                        }

                        Kirigami.AbstractCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            contentItem: ListView {
                                clip: true
                                model: fileModel ? fileModel.visibleEntries : []

                                delegate: ItemDelegate {
                                    required property var modelData
                                    width: ListView.view.width
                                    highlighted: modelData.path === project.selectedPath
                                    padding: 0
                                    topPadding: 0
                                    bottomPadding: 0
                                    leftPadding: 0
                                    rightPadding: 0
                                    implicitHeight: Math.max(Kirigami.Units.gridUnit * 0.95,
                                                             contentItem.implicitHeight + root.compactRowSpacing)

                                    contentItem: RowLayout {
                                        spacing: root.compactRowSpacing

                                        Item {
                                            Layout.preferredWidth: modelData.depth * root.compactMargin
                                            Layout.fillHeight: true
                                        }

                                        ToolButton {
                                            visible: modelData.isDir && modelData.hasChildren
                                            text: modelData.expanded ? "-" : "+"
                                            font.pointSize: root.compactSmallFontSize
                                            padding: 0
                                            implicitWidth: Kirigami.Units.gridUnit * 1.1
                                            implicitHeight: Kirigami.Units.gridUnit * 0.9
                                            onClicked: fileModel.toggleExpanded(modelData.path)
                                        }

                                        Kirigami.Icon {
                                            source: root.iconForFileType(modelData.fileType, modelData.isDir)
                                            implicitWidth: Kirigami.Units.iconSizes.small
                                            implicitHeight: Kirigami.Units.iconSizes.small
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            elide: Text.ElideMiddle
                                            font.pointSize: root.compactFontSize
                                            verticalAlignment: Text.AlignVCenter
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
                        SplitView.fillHeight: true
                        SplitView.preferredWidth: root.width * 0.34
                        SplitView.minimumWidth: Kirigami.Units.gridUnit * 10

                        contentItem: ColumnLayout {
                            spacing: root.compactSpacing

                            Kirigami.Heading {
                                text: "Overview"
                                level: 2
                            }

                            Label {
                                text: project.selectedFileData.fileName || "No selection"
                                font.bold: true
                                elide: Text.ElideMiddle
                                font.pointSize: root.compactFontSize
                            }

                            Label {
                                text: project.selectedFileData.summary || "Select a file to inspect its structure."
                                color: Kirigami.Theme.disabledTextColor
                                wrapMode: Text.WordWrap
                                font.pointSize: root.compactSmallFontSize
                            }

                            Label {
                                visible: project.selectedFileData.path
                                text: project.selectedRelativePath || ""
                                color: Kirigami.Theme.disabledTextColor
                                elide: Text.ElideMiddle
                                font.pointSize: root.compactSmallFontSize
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
                                        spacing: root.compactRowSpacing

                                        RowLayout {
                                            Layout.fillWidth: true

                                            Label {
                                                text: modelData.kind
                                                color: Kirigami.Theme.highlightColor
                                                font.pointSize: root.compactSmallFontSize
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: modelData.name
                                                font.bold: true
                                                elide: Text.ElideMiddle
                                                font.pointSize: root.compactFontSize
                                            }

                                            Label {
                                                text: "L" + modelData.line
                                                color: Kirigami.Theme.disabledTextColor
                                                font.pointSize: root.compactSmallFontSize
                                            }
                                        }

                                        Button {
                                            text: "Inspect"
                                            font.pointSize: root.compactSmallFontSize
                                            onClicked: project.selectSymbolByData(modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Kirigami.AbstractCard {
                        SplitView.fillHeight: true
                        SplitView.preferredWidth: root.width * 0.34
                        SplitView.minimumWidth: Kirigami.Units.gridUnit * 10

                        contentItem: ScrollView {
                            clip: true

                            ColumnLayout {
                                width: parent.width
                                spacing: root.compactSpacing

                                Kirigami.Heading {
                                    text: "Detail"
                                    level: 2
                                }

                                Label {
                                    visible: !!project.selectedSymbol.name
                                    text: project.selectedSymbol.kind ? project.selectedSymbol.kind + ": " + project.selectedSymbol.name : ""
                                    font.bold: true
                                    wrapMode: Text.WordWrap
                                    font.pointSize: root.compactFontSize
                                }

                                Label {
                                    visible: !!project.selectedSymbol.detail
                                    text: project.selectedSymbol.detail || ""
                                    color: Kirigami.Theme.disabledTextColor
                                    wrapMode: Text.WordWrap
                                    font.pointSize: root.compactSmallFontSize
                                }

                                Repeater {
                                    model: project.selectedSymbolMembers || []

                                    delegate: RowLayout {
                                        required property var modelData
                                        width: parent.width

                                        Label {
                                            text: modelData.kind
                                            color: Kirigami.Theme.highlightColor
                                            font.pointSize: root.compactSmallFontSize
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            elide: Text.ElideMiddle
                                            font.pointSize: root.compactFontSize
                                        }

                                        Label {
                                            text: "L" + modelData.line
                                            color: Kirigami.Theme.disabledTextColor
                                            font.pointSize: root.compactSmallFontSize
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
                                        font.pointSize: root.compactSmallFontSize
                                        enabled: !!modelData.snippet || (!!modelData.path && modelData.exists)
                                        onClicked: {
                                            if (modelData.snippet) {
                                                project.selectSymbolByData({
                                                    "kind": "dependency",
                                                    "name": modelData.label || modelData.target || "",
                                                    "sourcePath": project.selectedFileData.path || "",
                                                    "sourceLanguage": project.selectedFileData.language || "",
                                                    "line": modelData.line || 0,
                                                    "snippet": modelData.snippet || "",
                                                    "detail": modelData.detail || (modelData.type ? modelData.type + " dependency" : "dependency")
                                                })
                                            } else if (modelData.path && modelData.exists) {
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

                                    delegate: Button {
                                        required property var modelData
                                        width: parent.width
                                        text: (modelData.method || "ROUTE") + " "
                                              + (modelData.path || "")
                                              + (modelData.line ? " (L" + modelData.line + ")" : "")
                                        font.pointSize: root.compactSmallFontSize
                                        onClicked: {
                                            project.selectSymbolByData({
                                                "kind": "route",
                                                "name": modelData.label || ((modelData.method || "ROUTE") + " " + (modelData.path || "")),
                                                "sourcePath": project.selectedFileData.path || "",
                                                "sourceLanguage": project.selectedFileData.language || "",
                                                "line": modelData.line || 0,
                                                "snippet": modelData.snippet || "",
                                                "detail": modelData.detail || "route"
                                            })
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
                                        font.pointSize: root.compactSmallFontSize
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
                                    font.pointSize: root.compactFontSize
                                }

                                Label {
                                    visible: !!project.selectedFileData.packageSummary && !!project.selectedFileData.packageSummary.main
                                    text: "Main: " + ((project.selectedFileData.packageSummary && project.selectedFileData.packageSummary.main)
                                                      ? project.selectedFileData.packageSummary.main : "")
                                    Layout.fillWidth: true
                                    color: Kirigami.Theme.disabledTextColor
                                    wrapMode: Text.WordWrap
                                    font.pointSize: root.compactSmallFontSize
                                }

                                Label {
                                    visible: !!project.selectedFileData.packageSummary
                                             && (project.selectedFileData.packageSummary.dependencies || []).length > 0
                                    text: "Dependencies: "
                                          + ((project.selectedFileData.packageSummary && project.selectedFileData.packageSummary.dependencies)
                                             ? project.selectedFileData.packageSummary.dependencies.length : 0)
                                    color: Kirigami.Theme.disabledTextColor
                                    font.pointSize: root.compactSmallFontSize
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
                                            font.pointSize: root.compactSmallFontSize
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
                                            font.pointSize: root.compactSmallFontSize
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
                                        font.pointSize: root.compactSmallFontSize
                                        onClicked: {
                                            project.selectSymbolByData({
                                                "kind": "quick-link",
                                                "name": modelData.label || modelData.target || "",
                                                "sourcePath": project.selectedFileData.path || "",
                                                "sourceLanguage": project.selectedFileData.language || "",
                                                "line": modelData.line || 0,
                                                "snippet": modelData.snippet || "",
                                                "detail": modelData.detail || (modelData.exists ? "linked asset" : "missing linked asset")
                                            })
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
                                    font.pointSize: root.compactSmallFontSize
                                    text: "Matched"
                                }

                                Repeater {
                                    model: project.selectedFileData.cssSummary
                                           ? (project.selectedFileData.cssSummary.matchedClasses || [])
                                           : []

                                    delegate: ColumnLayout {
                                        required property var modelData
                                        width: parent.width
                                        spacing: 1

                                        Button {
                                            Layout.fillWidth: true
                                            text: modelData.name + (modelData.line ? " (L" + modelData.line + ")" : "")
                                            font.pointSize: root.compactSmallFontSize
                                            onClicked: project.selectSymbolByData({
                                                "kind": modelData.kind || "class",
                                                "name": modelData.name || "",
                                                "path": modelData.path || "",
                                                "language": "css",
                                                "line": modelData.line || 0,
                                                "snippet": modelData.snippet || "",
                                                "detail": "CSS class"
                                            })
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.snippet || ""
                                            color: Kirigami.Theme.disabledTextColor
                                            elide: Text.ElideRight
                                            font.family: "monospace"
                                            font.pointSize: root.compactSmallFontSize
                                        }
                                    }
                                }

                                Label {
                                    visible: !!project.selectedFileData.cssSummary
                                             && (project.selectedFileData.cssSummary.missingClasses || []).length > 0
                                    Layout.fillWidth: true
                                    color: "#f7a072"
                                    font.pointSize: root.compactSmallFontSize
                                    text: "Missing"
                                }

                                Repeater {
                                    model: project.selectedFileData.cssSummary
                                           ? (project.selectedFileData.cssSummary.missingClasses || [])
                                           : []

                                    delegate: ColumnLayout {
                                        required property var modelData
                                        width: parent.width
                                        spacing: 1

                                        Button {
                                            Layout.fillWidth: true
                                            text: modelData.name
                                            font.pointSize: root.compactSmallFontSize
                                            onClicked: project.selectSymbolByData({
                                                "kind": modelData.kind || "class",
                                                "name": modelData.name || "",
                                                "path": modelData.path || "",
                                                "language": "css",
                                                "line": modelData.line || 0,
                                                "snippet": modelData.snippet || "",
                                                "detail": "Missing CSS class"
                                            })
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.snippet || ""
                                            color: Kirigami.Theme.disabledTextColor
                                            elide: Text.ElideRight
                                            font.family: "monospace"
                                            font.pointSize: root.compactSmallFontSize
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Kirigami.AbstractCard {
                    SplitView.fillWidth: true
                    SplitView.preferredHeight: root.height * 0.22
                    SplitView.minimumHeight: Kirigami.Units.gridUnit * 6

                    contentItem: ColumnLayout {
                        spacing: root.compactSpacing

                        FontMetrics {
                            id: codeMetrics
                            font.family: "monospace"
                            font.pointSize: root.compactSmallFontSize
                        }

                        RowLayout {
                            Layout.fillWidth: true

                            Kirigami.Heading {
                                text: "Source"
                                level: 2
                            }

                            Label {
                                Layout.fillWidth: true
                                text: project.selectedSnippet.path
                                      ? root.snippetRelativePath(project.selectedSnippet.path)
                                        + (project.selectedSnippet.name
                                           ? "  " + ((project.selectedSnippet.kind || "snippet") + ": " + project.selectedSnippet.name)
                                           : "")
                                      : "No snippet selected"
                                color: Kirigami.Theme.disabledTextColor
                                elide: Text.ElideMiddle
                                horizontalAlignment: Text.AlignRight
                                font.pointSize: root.compactSmallFontSize
                            }

                            Label {
                                visible: !!project.selectedSnippet.detail
                                text: project.selectedSnippet.detail || ""
                                color: Kirigami.Theme.disabledTextColor
                                elide: Text.ElideRight
                                font.pointSize: root.compactSmallFontSize
                            }

                            Label {
                                visible: !!project.selectedSnippet.isTruncated
                                text: "preview"
                                color: "#f7a072"
                                font.pointSize: root.compactSmallFontSize
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            visible: !!project.selectedSnippet.path

                            Item {
                                Layout.fillWidth: true
                            }

                            Label {
                                visible: !!project.selectedSnippet.line
                                text: "L" + project.selectedSnippet.line
                                color: Kirigami.Theme.highlightColor
                                font.pointSize: root.compactSmallFontSize
                            }

                            Label {
                                visible: !!project.selectedSnippet.language
                                text: project.selectedSnippet.language
                                color: Kirigami.Theme.disabledTextColor
                                font.pointSize: root.compactSmallFontSize
                            }
                        }

                        Repeater {
                            model: project.selectedSnippet.diagnostics || []

                            delegate: Label {
                                required property var modelData
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: root.diagnosticColor(modelData.severity)
                                font.pointSize: root.compactSmallFontSize
                                text: (modelData.severity ? modelData.severity.toUpperCase() + ": " : "")
                                      + modelData.message
                                      + (modelData.line ? " (L" + modelData.line
                                                         + (modelData.column ? ", C" + modelData.column : "")
                                                         + ")" : "")
                            }
                        }

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true

                            TextEdit {
                                readOnly: true
                                selectByMouse: true
                                persistentSelection: true
                                textFormat: TextEdit.RichText
                                text: project.selectedSnippet.displayHtml || ""
                                color: "#d8dee9"
                                selectionColor: Kirigami.Theme.highlightColor
                                selectedTextColor: "#10141b"
                                wrapMode: TextEdit.NoWrap
                                font.family: "monospace"
                                font.pointSize: root.compactSmallFontSize
                                tabStopDistance: codeMetrics.averageCharacterWidth * 2
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

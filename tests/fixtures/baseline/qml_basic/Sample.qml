import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: root
    width: 320
    height: 200

    property string title: "Hello"
    signal accepted(string message)

    function greet(name) {
        return "Hello " + name
    }

    Button {
        id: actionButton
        text: root.title
        onClicked: root.accepted(greet("world"))
    }
}

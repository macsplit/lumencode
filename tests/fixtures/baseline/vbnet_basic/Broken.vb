Public Class Broken
    Public Sub First()
        Helper()
    ' End Sub is missing here

    Public Function Second() As Integer
        Return Helper()
    End Function

    Private Function Helper() As Integer
        Return 1
    End Function
End Class

Public Class ReportBuilder
    Public Function BuildHtml() As String
        Dim html As New System.Text.StringBuilder()
        html.Append("<script>
function updateHiddenInvoices() {
    return ""End Function"";
}
</script>")
        Return html.ToString()
    End Function

    Public Function AfterString() As String
        Return "ok"
    End Function
End Class

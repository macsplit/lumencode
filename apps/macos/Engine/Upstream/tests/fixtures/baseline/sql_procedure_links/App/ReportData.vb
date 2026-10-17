Public Class ReportData
    Public Sub LoadInvoices()
        Dim command As New SqlCommand()
        command.CommandType = CommandType.StoredProcedure
        command.CommandText = "dbo.Invoice_GetByGarage"
    End Sub
End Class

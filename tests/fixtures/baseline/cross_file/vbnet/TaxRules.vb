Public Module TaxRules
    Public Function VatFor(ByVal amount As Decimal) As Decimal
        Return amount * 0.2D
    End Function
End Module

Public Class Receipt
    Public Sub New(ByVal total As Decimal)
    End Sub
End Class

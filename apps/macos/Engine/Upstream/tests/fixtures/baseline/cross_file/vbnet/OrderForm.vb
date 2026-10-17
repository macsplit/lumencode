Public Class OrderForm
    Public Function Checkout(ByVal amount As Decimal) As Receipt
        ' VatFor(0) in a comment is not a call
        Dim vat As Decimal = TaxRules.VatFor(amount)
        Return New Receipt(amount + vat)
    End Function
End Class

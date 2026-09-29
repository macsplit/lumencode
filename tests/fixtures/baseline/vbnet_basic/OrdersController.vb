Imports Microsoft.AspNetCore.Mvc

<Route("api/orders")>
Public Class OrdersController
    Inherits ControllerBase

    <HttpGet("")>
    Public Function List() As IActionResult
        Return Ok()
    End Function

    <HttpPost("{id}/ship")>
    Public Function Ship(id As Integer) As IActionResult
        Return Ok(id)
    End Function
End Class

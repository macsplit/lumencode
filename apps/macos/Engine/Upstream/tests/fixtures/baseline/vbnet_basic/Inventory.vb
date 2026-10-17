Imports System.Collections.Generic
Imports Db = System.Data.SqlClient

Namespace Shop
    Public Class Inventory
        Inherits BaseStore
        Implements IDisposable

        Private ReadOnly _items As New List(Of String)
        Public Const MaxItems As Integer = 100

        Public Event Changed(sender As Object)

        Public Property Name As String

        Public ReadOnly Property Count As Integer
            Get
                Return _items.Count
            End Get
        End Property

        Public Sub New(ByVal name As String)
            Me.Name = name
        End Sub

        Public Function Add(ByVal item As String, Optional ByVal quantity As Integer = 1) As Boolean
            If Count >= MaxItems Then Return False
            _items.Add(item)
            Notify()
            Return True
        End Function

        Private Sub Notify()
            RaiseEvent Changed(Me)
        End Sub

        Public Sub Dispose() Implements IDisposable.Dispose
            _items.Clear()
        End Sub
    End Class

    Public Enum Status
        Active = 1
        Archived
    End Enum
End Namespace

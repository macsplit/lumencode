USE [ShopDb]
GO

-- Customers and their orders (SQL Server)
CREATE TABLE [dbo].[Customers] (
    [CustomerId] INT IDENTITY(1,1) NOT NULL,
    [Name] NVARCHAR(100) NOT NULL,
    [Email] NVARCHAR(200) NULL,
    CONSTRAINT [PK_Customers] PRIMARY KEY CLUSTERED ([CustomerId])
);
GO

CREATE TABLE dbo.Orders (
    OrderId INT IDENTITY(1,1) PRIMARY KEY,
    CustomerId INT NOT NULL REFERENCES dbo.Customers(CustomerId),
    Total DECIMAL(10, 2) NOT NULL DEFAULT 0,
    PlacedAt DATETIME2 NOT NULL
);
GO

CREATE OR ALTER FUNCTION dbo.fn_OrderCount (@CustomerId INT)
RETURNS INT
AS
BEGIN
    RETURN (SELECT COUNT(*) FROM dbo.Orders WHERE CustomerId = @CustomerId);
END
GO

CREATE PROCEDURE dbo.usp_PlaceOrder
    @CustomerId INT,
    @Total DECIMAL(10, 2) = 0,
    @OrderId INT OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    INSERT INTO dbo.Orders (CustomerId, Total, PlacedAt) VALUES (@CustomerId, @Total, SYSUTCDATETIME());
    SET @OrderId = SCOPE_IDENTITY();
    EXEC dbo.usp_LogOrder @OrderId = @OrderId;
    SELECT dbo.fn_OrderCount(@CustomerId) AS OrderCount;
END
GO

CREATE PROCEDURE dbo.usp_LogOrder
    @OrderId INT
AS
    SELECT o.OrderId, c.Name FROM dbo.Orders o JOIN dbo.Customers c ON c.CustomerId = o.CustomerId WHERE o.OrderId = @OrderId;
GO

CREATE TRIGGER dbo.trg_Orders_Audit ON dbo.Orders AFTER INSERT, UPDATE AS
    UPDATE dbo.Customers SET Email = Email WHERE CustomerId IN (SELECT CustomerId FROM inserted);
GO

CREATE NONCLUSTERED INDEX IX_Orders_Customer ON dbo.Orders (CustomerId);
GO

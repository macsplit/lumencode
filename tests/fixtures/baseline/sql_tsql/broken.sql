CREATE TABLE Items (
    ItemId INT PRIMARY KEY,
    Title NVARCHAR(50)
);
GO

CREATE PROCEDURE dbo.usp_Broken
    @Id INT
AS
BEGIN
    SELECT * FROM Items WHERE ItemId = @Id
    -- missing END, and a stray quote in code:
    PRINT 'unterminated
GO

CREATE PROCEDURE dbo.usp_AfterBroken
AS
    SELECT Title FROM Items;
GO

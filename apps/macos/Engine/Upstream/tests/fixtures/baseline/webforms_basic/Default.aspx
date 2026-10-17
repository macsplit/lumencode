<%@ Page Language="C#" CodeBehind="Default.aspx.cs" Inherits="Sample.Default" %>
<%@ Register TagPrefix="widgets" TagName="SavePanel" Src="~/Controls/SavePanel.ascx" %>
<%@ Import Namespace="Sample.Services" %>
<!doctype html>
<html>
<body>
  <form id="mainForm" runat="server">
    <asp:Content ID="BodyContent" ContentPlaceHolderID="MainContent" runat="server">
      <asp:Button ID="SaveButton" runat="server" Text="Save" OnClick="SaveButton_Click" />
      <span><%= Title %></span>
    </asp:Content>
  </form>
  <script runat="server" language="C#">
    protected void SaveButton_Click(object sender, EventArgs e) { }
  </script>
</body>
</html>

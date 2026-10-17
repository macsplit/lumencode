using Microsoft.UI.Xaml;

namespace LumenCode.Windows;

public partial class App : Application
{
    private Window? window;
    public App()
    {
        UnhandledException += (_, args) => RecordStartupError(args.Exception);
        InitializeComponent();
    }
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            if (Environment.GetCommandLineArgs().Contains("--gui-audit"))
            {
                DebugSettings.IsXamlResourceReferenceTracingEnabled = true;
                DebugSettings.XamlResourceReferenceFailed += (_, failure) => RecordDiagnostic(failure.Message);
                DebugSettings.IsBindingTracingEnabled = true;
                DebugSettings.BindingFailed += (_, failure) => RecordDiagnostic(failure.Message);
            }
            window = new MainWindow();
            window.Activate();
        }
        catch (Exception error) { RecordStartupError(error); Exit(); }
    }

    private static void RecordDiagnostic(string message)
    {
        var args = Environment.GetCommandLineArgs();
        int position = Array.IndexOf(args, "--gui-audit");
        if (position < 0 || args.Length <= position + 2) return;
        try
        {
            string folder = Path.GetFullPath(args[position + 2]);
            Directory.CreateDirectory(folder);
            File.AppendAllText(Path.Combine(folder, "xaml-diagnostics.txt"), message + Environment.NewLine);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static void RecordStartupError(Exception error)
    {
        try
        {
            string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LumenCode");
            var args = Environment.GetCommandLineArgs();
            int position = Array.IndexOf(args, "--gui-audit");
            if (position >= 0 && args.Length > position + 2) folder = Path.GetFullPath(args[position + 2]);
            Directory.CreateDirectory(folder);
            File.WriteAllText(Path.Combine(folder, "startup-error.txt"), error.ToString());
            if (position >= 0) File.WriteAllText(Path.Combine(folder, "gui.json"), System.Text.Json.JsonSerializer.Serialize(new { passed = false, failure = error.ToString(), stage = "startup" }));
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
}

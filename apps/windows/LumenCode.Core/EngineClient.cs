using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace LumenCode.Core;

/// <summary>Transport and presentation only. All language behavior lives in the shared engine.</summary>
public sealed class EngineClient(string executable, string? cacheDirectory = null)
{
    private readonly SemaphoreSlim slots = new(2);
    // The shared index writer uses one temporary file per cache path.
    private readonly SemaphoreSlim projectSlot = new(1);
    private readonly string cacheRoot = cacheDirectory ?? Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LumenCode", "cache");

    public static string FindExecutable()
    {
        string? configured = Environment.GetEnvironmentVariable("LUMENCODE_ENGINE");
        if (!string.IsNullOrWhiteSpace(configured)) return Path.GetFullPath(configured);
        return Path.Combine(AppContext.BaseDirectory, "EngineRuntime", "lumencode-engine.exe");
    }

    private string CachePath(string root)
    {
        Directory.CreateDirectory(cacheRoot);
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(root))));
        return Path.Combine(cacheRoot, key + ".json");
    }

    private string[] ProjectArguments(string root, params string[] operation) =>
        ["--root", root, "--cache", CachePath(root), .. operation];

    public Task<JsonElement> AnalyzeAsync(string path, string? root, CancellationToken token = default) =>
        root is null ? RunAsync(["--dump-file", path], TimeSpan.FromSeconds(10), token) :
            ProjectAsync(root, ["--path", path], TimeSpan.FromSeconds(45), token);

    public Task<JsonElement> BuildIndexAsync(string root, CancellationToken token = default) =>
        ProjectAsync(root, ["--build-index"], TimeSpan.FromMinutes(2), token);

    public Task<JsonElement> SearchAsync(string root, string query, string kinds = "", CancellationToken token = default) =>
        ProjectAsync(root, ["--search", query, "--kind", kinds], TimeSpan.FromSeconds(45), token);

    private async Task<JsonElement> ProjectAsync(string root, string[] operation, TimeSpan timeout, CancellationToken token)
    {
        await projectSlot.WaitAsync(token).ConfigureAwait(false);
        try { return await RunAsync(ProjectArguments(root, operation), timeout, token).ConfigureAwait(false); }
        finally { projectSlot.Release(); }
    }

    public Task<JsonElement> RunAsync(IReadOnlyList<string> arguments, TimeSpan timeout, CancellationToken token = default) =>
        Task.Run(async () =>
        {
            await slots.WaitAsync(token).ConfigureAwait(false);
            try { return await RunProcessAsync(arguments, timeout, token).ConfigureAwait(false); }
            finally { slots.Release(); }
        }, token);

    private async Task<JsonElement> RunProcessAsync(IReadOnlyList<string> arguments, TimeSpan timeout, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        if (!File.Exists(executable)) throw new FileNotFoundException("The analysis engine is missing. Rebuild the Windows package.", executable);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(timeout);
        var info = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = Path.GetDirectoryName(Path.GetFullPath(executable))!
        };
        foreach (string argument in arguments) info.ArgumentList.Add(argument);
        using var process = new Process { StartInfo = info };
        bool started = false;
        void Stop()
        {
            try { if (started && !process.HasExited) process.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
        }
        try
        {
            started = process.Start();
            if (!started) throw new IOException("Could not start the analysis engine.");
            using var cancellation = deadline.Token.Register(Stop);
            var output = ReadBoundedAsync(process.StandardOutput.BaseStream, 32 * 1024 * 1024, deadline.Token, Stop);
            var errors = ReadBoundedAsync(process.StandardError.BaseStream, 64 * 1024, deadline.Token, Stop);
            await Task.WhenAll(output, errors, process.WaitForExitAsync(deadline.Token)).ConfigureAwait(false);
            token.ThrowIfCancellationRequested();
            if (process.ExitCode != 0)
                throw new IOException($"Analysis engine exited with status {process.ExitCode}: {Encoding.UTF8.GetString(await errors).Trim()}");
            using var json = JsonDocument.Parse(await output);
            if (json.RootElement.ValueKind != JsonValueKind.Object) throw new InvalidDataException("The analysis engine returned an invalid response.");
            return json.RootElement.Clone();
        }
        catch (OperationCanceledException) when (!token.IsCancellationRequested)
        {
            throw new TimeoutException("The analysis engine exceeded its time limit.");
        }
        finally
        {
            Stop();
            if (started) await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
        }
    }

    private static async Task<byte[]> ReadBoundedAsync(Stream stream, int limit, CancellationToken token, Action stop)
    {
        using var output = new MemoryStream();
        var buffer = new byte[64 * 1024];
        int read;
        while ((read = await stream.ReadAsync(buffer, token).ConfigureAwait(false)) > 0)
        {
            if (output.Length + read > limit)
            {
                stop();
                throw new InvalidDataException("Analysis engine output exceeded its size limit.");
            }
            output.Write(buffer, 0, read);
        }
        return output.ToArray();
    }
}

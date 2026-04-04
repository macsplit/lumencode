using System;

public class Broken
{
    private static string HelperBrokenCs()
    {
        return "hello";
    }

    public string GreetBrokenCs()
    {
        return HelperBrokenCs();
    // Intentionally missing the closing brace for GreetBrokenCs.

    public string FallbackBrokenCs()
    {
        return HelperBrokenCs();
    }
}

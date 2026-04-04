using System;

public class Sample
{
    public string Name { get; }

    private static string Helper()
    {
        return "hello";
    }

    public Sample(string name)
    {
        Name = name;
    }

    public string Greet()
    {
        return Helper() + Name;
    }
}

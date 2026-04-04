import java.util.List;

public class Broken {
    private static String helperBrokenJava() {
        return "hello";
    }

    public String greetBrokenJava() {
        return helperBrokenJava();
    // Intentionally missing the closing brace for greetBrokenJava.

    public String fallbackBrokenJava() {
        return helperBrokenJava();
    }
}

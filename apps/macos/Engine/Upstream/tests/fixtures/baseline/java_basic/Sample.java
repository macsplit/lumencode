import java.util.List;

public class Sample {
    private String name;

    private static String helper() {
        return "hello";
    }

    public Sample(String name) {
        this.name = name;
    }

    public String greet() {
        return helper() + name;
    }
}

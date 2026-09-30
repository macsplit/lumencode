package billing;

public class Invoice {
    private final int amount;

    public Invoice(int amount) {
        this.amount = amount;
    }

    public static Invoice zeroInvoice() {
        return new Invoice(0);
    }

    public int amount() {
        return amount;
    }
}

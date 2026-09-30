package billing;

public class Billing {
    public int bill(int amount) {
        Invoice invoice = new Invoice(amount);
        Invoice empty = Invoice.zeroInvoice();
        return invoice.amount() + empty.amount();
    }
}

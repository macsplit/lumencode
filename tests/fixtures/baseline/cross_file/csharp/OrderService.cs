using Shop.Utils;

namespace Shop
{
    public class OrderService
    {
        public string Place(string order)
        {
            Guard.NotNull(order);
            return order;
        }
    }
}

const { total } = require('./cart');

describe('cart', () => {
    beforeEach(() => {});

    it('sums prices', () => {
        expect(total([{ price: 2 }, { price: 3 }])).toBe(5);
    });

    describe('empty', () => {
        test('is zero', () => {
            expect(total([])).toBe(0);
        });
    });
});

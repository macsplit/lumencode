# frozen_string_literal: true

module Shop
  # Prices "in cents" - a # in a string is not a comment
  class Pricing < Base
    TAX_RATE = 0.2
    attr_reader :currency, :rounding

    def initialize(currency = 'EUR', rounding: :half_up)
      @currency = currency
      @rounding = rounding
    end

    def total(items, discount = 0)
      subtotal = items.sum { |item| item[:price] }
      with_tax(subtotal - discount)
    end

    def with_tax(amount) = (amount * (1 + TAX_RATE)).round

    def self.default
      new
    end

    class << self
      def for_region(region)
        region == :eu ? new('EUR') : new('USD')
      end
    end

    private

    def describe
      <<~TEXT
        def not_a_method
        end
      TEXT
    end
  end
end

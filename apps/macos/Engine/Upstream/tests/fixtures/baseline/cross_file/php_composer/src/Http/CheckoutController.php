<?php

namespace Shop\Http;

use Shop\Billing\InvoiceMailer as Mailer;

class CheckoutController
{
    public function complete(string $email): bool
    {
        return Mailer::send($email);
    }
}

<?php

namespace App;

use App\Mail\Mailer;

class Signup
{
    public function register(string $email): bool
    {
        return Mailer::deliver($email);
    }
}

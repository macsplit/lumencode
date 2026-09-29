<?php
namespace App\Service;

class Mailer
{
    public function send(string $to): bool
    {
        return $to !== '';
    }
}

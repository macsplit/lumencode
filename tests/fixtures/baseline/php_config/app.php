<?php

// Laravel-style configuration
return [
    'name' => env('APP_NAME', 'Shop'),
    'debug' => (bool) env('APP_DEBUG', false),
    'providers' => [
        App\Providers\AppServiceProvider::class,
    ],
    'mail' => [
        'driver' => 'smtp',
        'from' => ['address' => 'shop@example.com', 'name' => 'Shop'],
    ],
];

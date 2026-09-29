<?php
declare(strict_types=1);

use App\Service\Mailer;
use Psr\Http\Message\ResponseInterface as Response;

$settings = require __DIR__ . '/../config/settings.php';
require_once 'helpers.php';

$app->get('/users/{id}', function ($request, Response $response) {
    return $response;
});
$app->map(['GET', 'POST'], '/contact', function ($request, $response) {
    (new Mailer())->send('team@example.com');
    return $response;
});
Route::post('/login', 'AuthController@login');
?>
<html>
<head><link rel="stylesheet" href="css/site.css"></head>
<body><div id="flash" class="flash"><?= h($message) ?></div><script src="js/site.js"></script></body>
</html>

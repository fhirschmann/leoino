<?php
/**
 * ESPuino configuration-backup receiver.
 *
 *   POST backup.php   (body = backup JSON, header X-ESPuino-Host: <hostname>)
 *       -> stores ./backups/<host>/<host>_<UTC timestamp>.json and prunes old ones
 *
 * The ESPuino's auto-backup uploads the same JSON the web interface's "export backup" button
 * produces (settings + RFID assignments + EQ rules + listening statistics, never any passwords).
 * The stored file can be fed straight back into the web interface's "import backup".
 *
 * Deploy: place this at the web root that serves your sync domain (next to manifest.php). Backups
 * land in ./backups (must be writable by PHP-FPM). Point the ESPuino's backup URL at
 * https://<host>/backup.php and protect it with the same basic auth as the file sync.
 *
 * nginx (e.g. SWAG) — route to PHP-FPM and protect with basic auth:
 *     location = /backup.php {
 *         auth_basic "Restricted";
 *         auth_basic_user_file /config/nginx/.htpasswd_espuino;
 *         client_max_body_size 8m;            # backups with many RFID tags can be large
 *         include /etc/nginx/fastcgi_params;
 *         fastcgi_param SCRIPT_FILENAME /path/to/espuino/backup.php;
 *         fastcgi_pass 127.0.0.1:9000;
 *     }
 */

header('Content-Type: application/json; charset=utf-8');

// How many backups to keep per device (older ones are pruned). 0 = keep all.
const KEEP_PER_HOST = 30;

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['error' => 'POST required']);
    exit;
}

$body = file_get_contents('php://input');
if ($body === false || strlen($body) === 0) {
    http_response_code(400);
    echo json_encode(['error' => 'empty body']);
    exit;
}

// Validate it is the expected backup JSON (cheap sanity check, not a full schema check).
$json = json_decode($body, true);
if (!is_array($json) || ($json['espuinoBackup'] ?? 0) != 1) {
    http_response_code(400);
    echo json_encode(['error' => 'not an ESPuino backup']);
    exit;
}

// Sanitize the device hostname into a safe directory/file component.
$host = $_SERVER['HTTP_X_ESPUINO_HOST'] ?? 'espuino';
$host = preg_replace('/[^A-Za-z0-9_.-]/', '_', $host);
if ($host === '' || $host === null) {
    $host = 'espuino';
}

$dir = __DIR__ . '/backups/' . $host;
if (!is_dir($dir) && !mkdir($dir, 0775, true) && !is_dir($dir)) {
    http_response_code(500);
    echo json_encode(['error' => 'cannot create backup directory']);
    exit;
}

$file = $dir . '/' . $host . '_' . gmdate('Ymd-His') . '.json';
if (file_put_contents($file, $body) === false) {
    http_response_code(500);
    echo json_encode(['error' => 'cannot write backup']);
    exit;
}

// Prune old backups, keeping the newest KEEP_PER_HOST.
if (KEEP_PER_HOST > 0) {
    $existing = glob($dir . '/*.json');
    if ($existing !== false && count($existing) > KEEP_PER_HOST) {
        sort($existing); // names sort chronologically (host_YYYYMMDD-HHMMSS.json)
        $remove = array_slice($existing, 0, count($existing) - KEEP_PER_HOST);
        foreach ($remove as $old) {
            @unlink($old);
        }
    }
}

echo json_encode(['ok' => true, 'stored' => basename($file), 'bytes' => strlen($body)]);

<?php
/**
 * ESPuino file-sync manifest (dynamic — there is no static manifest.json).
 *
 *   GET manifest.php -> {"version":1,"files":[{path,size},...]}
 *
 * Lists every audio/playlist file under ./sd so the ESPuino can pull missing/changed files.
 * RFID-tag syncing lives in the companion rfid.php (stateful store); this script is stateless.
 *
 * Deploy: place this at the web root that serves your sync domain. Audio files live in ./sd.
 * Point the ESPuino's file-sync URL at https://<host>/manifest.php. Needs PHP-FPM.
 *
 * nginx (e.g. SWAG) — route to PHP-FPM and protect with basic auth:
 *     root /path/to/espuino/sd;            # audio files are served from here
 *     location = /manifest.php {
 *         auth_basic "Restricted";
 *         auth_basic_user_file /config/nginx/.htpasswd_espuino;
 *         include /etc/nginx/fastcgi_params;
 *         fastcgi_param SCRIPT_FILENAME /path/to/espuino/manifest.php;
 *         fastcgi_pass 127.0.0.1:9000;
 *     }
 */

header('Content-Type: application/json; charset=utf-8');

$audioRoot = __DIR__ . '/sd';
$skip = ['manifest.json', 'manifest.php', 'rfid.php', 'rfid_master.json', '.DS_Store', 'Thumbs.db'];

$files = [];
if (is_dir($audioRoot)) {
    $it = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($audioRoot, RecursiveDirectoryIterator::SKIP_DOTS));
    foreach ($it as $f) {
        if (!$f->isFile()) {
            continue;
        }
        $name = $f->getFilename();
        $rel = str_replace(DIRECTORY_SEPARATOR, '/', substr($f->getPathname(), strlen($audioRoot) + 1));
        // skip listed names, AppleDouble files and anything under a hidden folder
        if (in_array($name, $skip, true) || strpos($name, '._') === 0 || strpos($rel, '/.') !== false || strpos($rel, '.') === 0) {
            continue;
        }
        $files[] = ['path' => $rel, 'size' => $f->getSize()];
    }
}
usort($files, function ($a, $b) { return strcmp($a['path'], $b['path']); });

echo json_encode(['version' => 1, 'files' => $files], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);

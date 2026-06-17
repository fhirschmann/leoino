<?php
/**
 * ESPuino sync endpoint (everything generated dynamically — there is no static manifest.json).
 *
 *   GET  manifest.php             -> {"version":1,"files":[{path,size},...],"rfid":[...]}   (file sync)
 *   GET  manifest.php?type=rfid   -> {"rfid":[...]}                                         (fast RFID pull)
 *   POST manifest.php             body: a single {id,...} entry, {"rfid":[...]} or a flat
 *                                 array -> merges into rfid_master.json (newest-wins by
 *                                 "timestamp") and returns {"status":"ok"}.
 *
 * RFID entry shape: {id, timestamp, fileOrUrl, playMode}  (music card)
 *                or {id, timestamp, modId}                 (modification card)
 *
 * Deploy: place this at the web root that serves your sync domain. Audio files live in ./sd.
 * The RFID store (rfid_master.json) is created next to this script on the first POST.
 */

header('Content-Type: application/json; charset=utf-8');

$rfidFile = __DIR__ . '/rfid_master.json';
$audioRoot = __DIR__ . '/sd';

// Load the RFID store as id => entry (flat array on disk).
function rfid_load($rfidFile) {
    $indexed = [];
    if (file_exists($rfidFile)) {
        $data = json_decode(file_get_contents($rfidFile), true);
        if (isset($data['rfid'])) {
            $data = $data['rfid'];
        }
        if (is_array($data)) {
            foreach ($data as $e) {
                if (isset($e['id'])) {
                    $indexed[(string)$e['id']] = $e;
                }
            }
        }
    }
    return $indexed;
}

// ---------- POST: merge incoming RFID assignments (newest-wins by timestamp) ----------
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $input = json_decode(file_get_contents('php://input'), true);
    $master = rfid_load($rfidFile);

    // Accept a single entry {id,...}, an ESPuino backup {"rfid":[...]}, or a flat array.
    $entries = [];
    if (isset($input['id'])) {
        $entries = [$input];
    } elseif (isset($input['rfid']) && is_array($input['rfid'])) {
        $entries = $input['rfid'];
    } elseif (is_array($input)) {
        foreach ($input as $item) {
            if (is_array($item) && isset($item['id'])) {
                $entries[] = $item;
            }
        }
    }

    foreach ($entries as $e) {
        if (!isset($e['id'])) {
            continue;
        }
        $id = (string)$e['id'];
        $ts = isset($e['timestamp']) ? (int)$e['timestamp'] : time();
        if (!isset($master[$id]) || $ts > (int)($master[$id]['timestamp'] ?? 0)) {
            $clean = ['id' => $id, 'timestamp' => $ts];
            if (isset($e['modId'])) {
                $clean['modId'] = (int)$e['modId'];
            } else {
                if (isset($e['fileOrUrl'])) {
                    $clean['fileOrUrl'] = $e['fileOrUrl'];
                }
                if (isset($e['playMode'])) {
                    $clean['playMode'] = (int)$e['playMode'];
                }
            }
            $master[$id] = $clean;
        }
    }

    $tmp = $rfidFile . '.tmp';
    file_put_contents($tmp, json_encode(array_values($master), JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE));
    @rename($tmp, $rfidFile);

    echo json_encode(['status' => 'ok']);
    exit;
}

// ---------- GET ?type=rfid: just the RFID list (no directory scan) ----------
if (isset($_GET['type']) && $_GET['type'] === 'rfid') {
    echo json_encode(['rfid' => array_values(rfid_load($rfidFile))], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    exit;
}

// ---------- GET (default): dynamic file manifest + RFID list ----------
$skip = ['manifest.json', 'manifest.php', '.DS_Store', 'Thumbs.db'];
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

echo json_encode([
    'version' => 1,
    'files'   => $files,
    'rfid'    => array_values(rfid_load($rfidFile)),
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);

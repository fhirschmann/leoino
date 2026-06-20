<?php
/**
 * ESPuino RFID-tag sync endpoint (stateful store; companion to manifest.php which lists files).
 *
 *   GET  rfid.php          -> {"rfid":[ {id, timestamp, fileOrUrl, playMode} | {id, timestamp, modId} ]}
 *   POST rfid.php          body: a single {id,...} entry, {"rfid":[...]} or a flat array
 *                          -> merges into rfid_master.json (newest-wins by "timestamp"),
 *                             returns {"status":"ok"}
 *
 * Deploy: place this in its own rfid/ folder under the web root; the store (rfid_master.json) is
 * created next to it on the first POST. Point the ESPuino's RFID-sync URL at https://<host>/rfid/rfid.php.
 * One web root above the service folders serves all three endpoints (see manifest.php / the
 * espuinosync.subdomain.conf example) with the standard PHP handler:
 *     root /path/to/espuino;               # listing at / shows sd/, rfid/, backup/
 *     location ~ \.php$ {
 *         auth_basic "Restricted";
 *         auth_basic_user_file /config/nginx/.htpasswd_espuino;
 *         include /etc/nginx/fastcgi_params;
 *         fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
 *         fastcgi_pass 127.0.0.1:9000;
 *     }
 */

header('Content-Type: application/json; charset=utf-8');

$rfidFile = __DIR__ . '/rfid_master.json';

// Load the store as id => entry (flat array on disk).
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

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $input = json_decode(file_get_contents('php://input'), true);

    // Serialize concurrent POSTs: load -> merge -> write is a read-modify-write, so without an
    // exclusive lock two devices syncing at the same time could clobber each other's updates
    // ("lost update"). The lock is held across the whole sequence until the atomic rename below.
    $lock = fopen($rfidFile . '.lock', 'c');
    if ($lock === false || !flock($lock, LOCK_EX)) {
        http_response_code(503);
        echo json_encode(['error' => 'cannot acquire lock']);
        exit;
    }

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
            if (!empty($e['deleted'])) {
                // tombstone: a deletion wins newest-wins like any other change
                $master[$id] = ['id' => $id, 'timestamp' => $ts, 'deleted' => true];
            } else {
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
    }

    // prune tombstones older than 120 days so the store doesn't grow forever
    $cutoff = time() - 120 * 86400;
    foreach ($master as $id => $e) {
        if (!empty($e['deleted']) && (int)($e['timestamp'] ?? 0) < $cutoff) {
            unset($master[$id]);
        }
    }

    $tmp = $rfidFile . '.tmp';
    file_put_contents($tmp, json_encode(array_values($master), JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE));
    @rename($tmp, $rfidFile);

    flock($lock, LOCK_UN);
    fclose($lock);

    echo json_encode(['status' => 'ok']);
    exit;
}

// GET: return the full tag list.
echo json_encode(['rfid' => array_values(rfid_load($rfidFile))], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);

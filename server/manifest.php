<?php
/**
 * ESPuino file-sync manifest + bidirectional playlist store.
 *
 *   GET    manifest.php
 *          -> {version:2, files:[{path,size,sha256?,revision?}], playlistTombstones:[...]}
 *   POST   manifest.php?playlist=Playlists/name.m3u&baseRevision=N
 *          body = raw M3U file; creates/updates it if N is still the current revision
 *   DELETE manifest.php?playlist=Playlists/name.m3u&baseRevision=N
 *          creates a persistent tombstone if N is still the current revision
 *
 * Audio remains server -> device. Only the flat Playlists/*.m3u[8] namespace is writable. The
 * hidden .playlist_sync.json store tracks a monotonically increasing server revision per path, so
 * clients can do a three-way merge without trusting their clocks. Existing firmware ignores the
 * extra fields and continues to pull the live playlist files from the normal manifest.
 *
 * Deploy this file inside the audio folder and point ESPuino's file-sync URL at it, e.g.
 * https://host/sd/manifest.php. Protect it with the same HTTP Basic Auth as the file downloads.
 */

header('Content-Type: application/json; charset=utf-8');

$audioRoot = __DIR__;
$playlistDir = $audioRoot . '/Playlists';
$stateFile = $audioRoot . '/.playlist_sync.json';
$lockFile = $audioRoot . '/.playlist_sync.lock';
$maxPlaylistBytes = 8 * 1024 * 1024;
$skip = [
    'manifest.json', 'manifest.php', 'rfid.php', 'rfid_master.json',
    '.playlist_sync.json', '.playlist_sync.lock', '.DS_Store', 'Thumbs.db',
];

function json_response($body, $status = 200) {
    http_response_code($status);
    echo json_encode($body, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

function playlist_normalize_path($input) {
    $path = str_replace('\\', '/', trim((string)$input));
    $path = ltrim($path, '/');
    if (strlen($path) === 0 || strlen($path) > 240 || strpos($path, '..') !== false) {
        return null;
    }
    if (!preg_match('~\APlaylists/([^/]+)\.m3u8?\z~i', $path, $match)) {
        return null;
    }
    if ($match[1] === '' || $match[1][0] === '.') {
        return null;
    }
    return $path;
}

function playlist_key($path) {
    // ESPuino's SD card uses FAT, so playlist paths are case-insensitive there as well.
    return strtolower($path);
}

function playlist_default_state() {
    return ['nextRevision' => 1, 'entries' => []];
}

function playlist_load_state($stateFile) {
    if (!is_file($stateFile)) {
        return playlist_default_state();
    }
    $decoded = json_decode((string)file_get_contents($stateFile), true);
    if (!is_array($decoded) || !isset($decoded['entries']) || !is_array($decoded['entries'])) {
        return playlist_default_state();
    }
    $decoded['nextRevision'] = max(1, (int)($decoded['nextRevision'] ?? 1));
    return $decoded;
}

function playlist_next_revision(&$state) {
    $revision = max(1, (int)$state['nextRevision']);
    $state['nextRevision'] = $revision + 1;
    return $revision;
}

function playlist_write_state($stateFile, $state) {
    $tmp = $stateFile . '.tmp';
    $json = json_encode($state, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    if ($json === false || file_put_contents($tmp, $json, LOCK_EX) === false || !@rename($tmp, $stateFile)) {
        @unlink($tmp);
        return false;
    }
    return true;
}

// Reconcile direct server-side edits into the revision store. This also imports existing playlists
// on the first request and turns files removed outside the API into tombstones.
function playlist_scan_server(&$state, $playlistDir) {
    $changed = false;
    $seen = [];
    if (is_dir($playlistDir)) {
        foreach (new DirectoryIterator($playlistDir) as $file) {
            if (!$file->isFile()) {
                continue;
            }
            $path = playlist_normalize_path('Playlists/' . $file->getFilename());
            if ($path === null) {
                continue;
            }
            $key = playlist_key($path);
            $seen[$key] = true;
            $hash = hash_file('sha256', $file->getPathname());
            $size = $file->getSize();
            $old = $state['entries'][$key] ?? null;
            if (!is_array($old) || !empty($old['deleted']) || ($old['sha256'] ?? '') !== $hash || (int)($old['size'] ?? -1) !== $size) {
                $state['entries'][$key] = [
                    'path' => $path,
                    'revision' => playlist_next_revision($state),
                    'sha256' => $hash,
                    'size' => $size,
                    'deleted' => false,
                ];
                $changed = true;
            } elseif (($old['path'] ?? '') !== $path) {
                $state['entries'][$key]['path'] = $path;
                $changed = true;
            }
        }
    }

    foreach ($state['entries'] as $key => $entry) {
        if (empty($entry['deleted']) && !isset($seen[$key])) {
            $state['entries'][$key] = [
                'path' => (string)($entry['path'] ?? $key),
                'revision' => playlist_next_revision($state),
                'deleted' => true,
            ];
            $changed = true;
        }
    }
    return $changed;
}

function playlist_validate_file($path, $maxBytes) {
    if (!is_file($path) || filesize($path) <= 0 || filesize($path) > $maxBytes) {
        return false;
    }
    $handle = fopen($path, 'rb');
    if ($handle === false) {
        return false;
    }
    $entries = 0;
    $valid = true;
    while (($line = fgets($handle, 2051)) !== false) {
        if (strlen($line) > 2048 && substr($line, -1) !== "\n") {
            $valid = false;
            break;
        }
        if (strpos($line, "\0") !== false) {
            $valid = false;
            break;
        }
        $trimmed = trim($line);
        if ($trimmed !== '' && $trimmed[0] !== '#') {
            $entries++;
            if ($entries > 4096) {
                $valid = false;
                break;
            }
        }
    }
    if (!feof($handle)) {
        $valid = false;
    }
    fclose($handle);
    return $valid && $entries > 0;
}

function playlist_conflict($entry) {
    json_response([
        'error' => 'playlist conflict',
        'path' => $entry['path'] ?? '',
        'revision' => (int)($entry['revision'] ?? 0),
        'sha256' => $entry['sha256'] ?? '',
        'deleted' => !empty($entry['deleted']),
    ], 409);
}

$method = strtoupper($_SERVER['REQUEST_METHOD'] ?? 'GET');
$playlistParam = $_GET['playlist'] ?? null;

if (($method === 'POST' || $method === 'DELETE') && $playlistParam !== null) {
    $path = playlist_normalize_path($playlistParam);
    if ($path === null) {
        json_response(['error' => 'invalid playlist path'], 400);
    }
    $baseRevision = max(0, (int)($_GET['baseRevision'] ?? 0));
    $lock = fopen($lockFile, 'c');
    if ($lock === false || !flock($lock, LOCK_EX)) {
        json_response(['error' => 'cannot acquire playlist lock'], 503);
    }

    $state = playlist_load_state($stateFile);
    $stateChanged = playlist_scan_server($state, $playlistDir);
    $key = playlist_key($path);
    $current = $state['entries'][$key] ?? null;
    $currentRevision = is_array($current) ? (int)($current['revision'] ?? 0) : 0;

    if ($method === 'POST') {
        if (!is_dir($playlistDir) && !mkdir($playlistDir, 0775, true) && !is_dir($playlistDir)) {
            flock($lock, LOCK_UN);
            fclose($lock);
            json_response(['error' => 'cannot create playlist directory'], 500);
        }
        $contentLength = isset($_SERVER['CONTENT_LENGTH']) ? (int)$_SERVER['CONTENT_LENGTH'] : -1;
        if ($contentLength === 0 || $contentLength > $maxPlaylistBytes) {
            flock($lock, LOCK_UN);
            fclose($lock);
            json_response(['error' => 'playlist is empty or too large'], 413);
        }

        // Preserve the server's canonical spelling when a FAT-backed client addresses an existing
        // playlist with different case. Otherwise a case-sensitive host could create two files for
        // the one path that ESPuino sees.
        $targetPath = is_array($current) && empty($current['deleted']) ? (string)$current['path'] : $path;
        $target = $audioRoot . '/' . $targetPath;
        $tmp = $target . '.upload-' . bin2hex(random_bytes(4));
        $in = fopen('php://input', 'rb');
        $out = fopen($tmp, 'xb');
        $hashContext = hash_init('sha256');
        $written = 0;
        $copyOk = ($in !== false && $out !== false);
        while ($copyOk && !feof($in)) {
            $chunk = fread($in, 8192);
            if ($chunk === false) {
                $copyOk = false;
                break;
            }
            if ($chunk === '') {
                continue;
            }
            $written += strlen($chunk);
            if ($written > $maxPlaylistBytes || fwrite($out, $chunk) !== strlen($chunk)) {
                $copyOk = false;
                break;
            }
            hash_update($hashContext, $chunk);
        }
        if (is_resource($in)) {
            fclose($in);
        }
        if (is_resource($out)) {
            fflush($out);
            fclose($out);
        }
        $incomingHash = $copyOk ? hash_final($hashContext) : '';

        if (!$copyOk || $written === 0 || !playlist_validate_file($tmp, $maxPlaylistBytes)) {
            @unlink($tmp);
            flock($lock, LOCK_UN);
            fclose($lock);
            json_response(['error' => 'invalid or incomplete playlist'], 400);
        }

        // A retry after a lost response is idempotent even if its base revision is now stale.
        if (is_array($current) && empty($current['deleted']) && ($current['sha256'] ?? '') === $incomingHash) {
            @unlink($tmp);
            if ($stateChanged) {
                playlist_write_state($stateFile, $state);
            }
            flock($lock, LOCK_UN);
            fclose($lock);
            json_response([
                'status' => 'unchanged', 'path' => $current['path'],
                'revision' => $currentRevision, 'sha256' => $incomingHash,
            ]);
        }
        if ($currentRevision !== $baseRevision) {
            @unlink($tmp);
            if ($stateChanged) {
                playlist_write_state($stateFile, $state);
            }
            flock($lock, LOCK_UN);
            fclose($lock);
            playlist_conflict($current ?? ['path' => $path, 'revision' => 0, 'deleted' => true]);
        }

        $backup = $target . '.bak';
        @unlink($backup);
        $hadTarget = is_file($target);
        if (($hadTarget && !@rename($target, $backup)) || !@rename($tmp, $target)) {
            @unlink($tmp);
            if ($hadTarget && is_file($backup)) {
                @rename($backup, $target);
            }
            flock($lock, LOCK_UN);
            fclose($lock);
            json_response(['error' => 'cannot replace playlist'], 500);
        }
        @unlink($backup);
        $revision = playlist_next_revision($state);
        $state['entries'][$key] = [
            'path' => $targetPath,
            'revision' => $revision,
            'sha256' => $incomingHash,
            'size' => $written,
            'deleted' => false,
        ];
        if (!playlist_write_state($stateFile, $state)) {
            flock($lock, LOCK_UN);
            fclose($lock);
            json_response(['error' => 'cannot persist playlist state'], 500);
        }
        flock($lock, LOCK_UN);
        fclose($lock);
        json_response(['status' => 'ok', 'path' => $targetPath, 'revision' => $revision, 'sha256' => $incomingHash], $hadTarget ? 200 : 201);
    }

    // DELETE: already-deleted is idempotent; otherwise reject a stale base revision.
    if (is_array($current) && !empty($current['deleted'])) {
        if ($stateChanged) {
            playlist_write_state($stateFile, $state);
        }
        flock($lock, LOCK_UN);
        fclose($lock);
        json_response(['status' => 'unchanged', 'path' => $current['path'], 'revision' => $currentRevision, 'deleted' => true]);
    }
    if ($currentRevision !== $baseRevision) {
        if ($stateChanged) {
            playlist_write_state($stateFile, $state);
        }
        flock($lock, LOCK_UN);
        fclose($lock);
        playlist_conflict($current ?? ['path' => $path, 'revision' => 0, 'deleted' => true]);
    }
    $targetPath = is_array($current) && empty($current['deleted']) ? (string)$current['path'] : $path;
    $target = $audioRoot . '/' . $targetPath;
    if (is_file($target) && !@unlink($target)) {
        flock($lock, LOCK_UN);
        fclose($lock);
        json_response(['error' => 'cannot delete playlist'], 500);
    }
    $revision = playlist_next_revision($state);
    $state['entries'][$key] = ['path' => $targetPath, 'revision' => $revision, 'deleted' => true];
    if (!playlist_write_state($stateFile, $state)) {
        flock($lock, LOCK_UN);
        fclose($lock);
        json_response(['error' => 'cannot persist playlist tombstone'], 500);
    }
    flock($lock, LOCK_UN);
    fclose($lock);
    json_response(['status' => 'ok', 'path' => $targetPath, 'revision' => $revision, 'deleted' => true]);
}

if ($method !== 'GET') {
    json_response(['error' => 'method not allowed'], 405);
}

// GET manifest: scan under the same lock so a concurrent upload cannot expose a file with stale
// metadata. Tombstones are deliberately retained; the number of playlists is tiny, and retaining
// them prevents an ESPuino that was offline for months from resurrecting a deleted playlist.
$lock = fopen($lockFile, 'c');
if ($lock === false || !flock($lock, LOCK_EX)) {
    json_response(['error' => 'cannot acquire playlist lock'], 503);
}
$state = playlist_load_state($stateFile);
$changed = playlist_scan_server($state, $playlistDir);
if ($changed && !playlist_write_state($stateFile, $state)) {
    flock($lock, LOCK_UN);
    fclose($lock);
    json_response(['error' => 'cannot persist playlist state'], 500);
}

$files = [];
if (is_dir($audioRoot)) {
    $it = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($audioRoot, RecursiveDirectoryIterator::SKIP_DOTS));
    foreach ($it as $file) {
        if (!$file->isFile()) {
            continue;
        }
        $name = $file->getFilename();
        $rel = str_replace(DIRECTORY_SEPARATOR, '/', substr($file->getPathname(), strlen($audioRoot) + 1));
        if (in_array($name, $skip, true) || strpos($name, '._') === 0 || strpos($rel, '/.') !== false || strpos($rel, '.') === 0 || preg_match('/\.(tmp|bak|upload-[a-f0-9]+)$/i', $name)) {
            continue;
        }
        $entry = ['path' => $rel, 'size' => $file->getSize()];
        $playlistPath = playlist_normalize_path($rel);
        if ($playlistPath !== null) {
            $stored = $state['entries'][playlist_key($playlistPath)] ?? null;
            if (is_array($stored) && empty($stored['deleted'])) {
                $entry['sha256'] = (string)$stored['sha256'];
                $entry['revision'] = (int)$stored['revision'];
            }
        }
        $files[] = $entry;
    }
}
usort($files, function ($a, $b) { return strcmp($a['path'], $b['path']); });

$tombstones = [];
foreach ($state['entries'] as $entry) {
    if (!empty($entry['deleted'])) {
        $tombstones[] = ['path' => $entry['path'], 'revision' => (int)$entry['revision']];
    }
}
usort($tombstones, function ($a, $b) { return strcmp($a['path'], $b['path']); });

flock($lock, LOCK_UN);
fclose($lock);

echo json_encode([
    'version' => 2,
    'files' => $files,
    'playlistTombstones' => $tombstones,
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);

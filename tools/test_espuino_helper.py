"""Host integration tests: credential selection, authenticated requests and private diagnostics."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('helper', Path(__file__).with_name('espuino_helper.py'))
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class HelperTest(unittest.TestCase):
    def test_credentials_and_precedence(self):
        with tempfile.TemporaryDirectory() as tmp:
            shared = Path(tmp) / 'secrets.json'
            shared.write_text(json.dumps({'services': {'leoino': {'url': 'http://box', 'api_key': 'shared'}}}))
            with patch.dict(os.environ, {'HOMELAB_SECRETS_PATH': str(shared)}, clear=True), patch.object(helper, 'CONFIG_FILE', str(Path(tmp) / 'missing')):
                self.assertEqual(helper.load_config(), ('box', 'espuino', 'shared'))
                with patch.dict(os.environ, {'ESPUINO_IP': 'override', 'ESPUINO_API_KEY': 'env'}):
                    self.assertEqual(helper.load_config(), ('override', 'espuino', 'env'))

    def test_requests_and_diagnostics(self):
        seen = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append((self.path, self.headers.get('X-API-Key')))
                if self.headers.get('X-API-Key') != 'test-key':
                    self.send_error(401)
                    return
                if self.path == '/redirect':
                    self.send_response(302)
                    self.send_header('Location', '/info')
                    self.end_headers()
                    return
                if self.path == '/coredump':
                    self.send_error(404)
                    return
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{}')
            def log_message(self, *args):
                pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with patch.object(helper, 'ESPUINO_IP', f'127.0.0.1:{server.server_port}'), patch.object(helper, 'PASS', 'test-key'), tempfile.TemporaryDirectory() as tmp:
                self.assertEqual(helper.api_request('/info'), b'{}')
                with self.assertRaises(Exception):
                    helper.api_request('/redirect')
                self.assertEqual(seen[-1][0], '/redirect')
                with patch.object(helper, 'PASS', 'bad'):
                    with self.assertRaises(Exception):
                        helper.api_request('/info')
                with contextlib.redirect_stdout(io.StringIO()) as out:
                    helper.download_diagnostics(tmp)
                self.assertNotIn('test-key', out.getvalue())
                self.assertIn('No stored core dump', out.getvalue())
                for name in ('info.json', 'debug.json', 'log.txt'):
                    path = Path(tmp) / name
                    self.assertEqual(path.read_bytes(), b'{}')
                    self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertFalse((Path(tmp) / 'coredump.bin').exists())
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()

"""Integration checks that start an isolated local terminal gateway per test."""
import http.client
import os
from pathlib import Path
import subprocess
import time
import tempfile
import socket
import unittest

HOST = '127.0.0.1:8035'
ORIGIN = 'http://' + HOST
CODE = '24681357'

class GatewayTests(unittest.TestCase):
    def setUp(self):
        global HOST, ORIGIN
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        HOST = f'127.0.0.1:{self.port}'
        ORIGIN = 'http://' + HOST
        self.temp = tempfile.TemporaryDirectory()
        binary = Path(__file__).resolve().parents[2] / 'build/ui-terminal-host'
        self.server = subprocess.Popen([str(binary), '/bin/sh', '-i'],
            env={**os.environ, 'PS5_UI_PAIR_CODE': CODE, 'PS5_UI_PORT': str(self.port), 'HOME': self.temp.name},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for attempt in range(100):
            try:
                with socket.create_connection(('127.0.0.1', self.port), timeout=.1): break
            except OSError:
                if self.server.poll() is not None: self.fail(self.server.stderr.read().decode())
                time.sleep(.01)
        else: self.fail('Terminal host did not start')

    def tearDown(self):
        self.server.terminate()
        self.server.communicate(timeout=5)
        self.temp.cleanup()

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
        conn.request(method, path, body, headers or {})
        response = conn.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        conn.close()
        return result

    def test_assets_have_content_security_policy(self):
        status, headers, body = self.request('GET', '/')
        self.assertEqual(status, 200)
        self.assertIn(b'PS5 AI CLI', body)
        self.assertIn("frame-ancestors 'none'", headers['Content-Security-Policy'])
        self.assertEqual(headers['Cache-Control'], 'no-store')

    def test_rebinding_host_rejected(self):
        self.assertEqual(self.request('GET', '/', headers={'Host': 'attacker.example:' + str(self.port)})[0], 403)

    def test_cross_origin_pair_rejected(self):
        headers = {'Origin': 'https://attacker.example', 'X-PS5-Client': '1'}
        self.assertEqual(self.request('POST', '/api/pair', CODE, headers)[0], 403)

    def test_missing_origin_pair_rejected(self):
        self.assertEqual(self.request('POST', '/api/pair', CODE, {'X-PS5-Client': '1'})[0], 403)

    def test_wrong_code_rejected(self):
        self.assertEqual(self.request('POST', '/api/pair', '99999999', {'Origin': ORIGIN, 'X-PS5-Client': '1'})[0], 401)

    def test_unpaired_websocket_rejected(self):
        self.assertEqual(self.request('GET', '/terminal/codex?cols=80&rows=24', headers={'Origin': ORIGIN})[0], 401)

    def test_pair_cookie_and_revoke(self):
        headers = {'Origin': ORIGIN, 'X-PS5-Client': '1'}
        status, result, _ = self.request('POST', '/api/pair', CODE, headers)
        self.assertEqual(status, 200)
        cookie = result['Set-Cookie']
        self.assertIn('HttpOnly', cookie)
        self.assertIn('SameSite=Strict', cookie)
        headers['Cookie'] = cookie.split(';')[0]
        self.assertIn(b'"paired":true', self.request('GET', '/api/status', headers=headers)[2])
        self.assertEqual(self.request('POST', '/api/unpair', '', headers)[0], 200)
        self.assertIn(b'"paired":false', self.request('GET', '/api/status', headers=headers)[2])

    def test_local_console_pairing_requires_origin_and_client_header(self):
        for headers in ({}, {'Origin': ORIGIN}, {'Origin': 'https://attacker.example', 'X-PS5-Client': '1'}):
            with self.subTest(headers=headers):
                self.assertEqual(self.request('POST', '/api/pair-local', '', headers)[0], 403)
        status, headers, _ = self.request('POST', '/api/pair-local', '', {'Origin': ORIGIN, 'X-PS5-Client': '1'})
        self.assertEqual(status, 200)
        cookie = headers['Set-Cookie'].split(';')[0]
        self.assertIn(b'"paired":true', self.request('GET', '/api/status', headers={'Cookie': cookie})[2])

    def test_connection_upgrade_must_be_a_header_token(self):
        _, result, _ = self.request('POST', '/api/pair', CODE, {'Origin': ORIGIN, 'X-PS5-Client': '1'})
        headers = {'Origin': ORIGIN, 'Cookie': result['Set-Cookie'].split(';')[0],
                   'Upgrade': 'websocket', 'Connection': 'not-an-upgrade',
                   'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ=='}
        self.assertEqual(self.request('GET', '/terminal/codex?cols=80&rows=24', headers=headers)[0], 403)

    def test_header_smuggling_rejected(self):
        for headers in ['Host: '+HOST+'\r\nHost: '+HOST,
                        'Host: '+HOST+'\r\nTransfer-Encoding: chunked',
                        'Host: '+HOST+'\r\nContent-Length: 0\r\nContent-Length: 0',
                        'Host: '+HOST+'\r\nContent-Length: 99999999999999999999999']:
            with self.subTest(headers=headers), socket.create_connection(('127.0.0.1', self.port), timeout=3) as s:
                s.sendall(('POST /api/pair HTTP/1.1\r\n'+headers+'\r\n\r\n').encode())
                self.assertIn(b'400', s.recv(4096).split(b'\r\n')[0])

    def test_no_arbitrary_file_serving(self):
        for path in ['/../LICENSE', '/%2e%2e/LICENSE', '/auth.json', '/api/status?extra=1']:
            with self.subTest(path=path):
                self.assertEqual(self.request('GET', path)[0], 404)

if __name__ == '__main__':
    unittest.main()

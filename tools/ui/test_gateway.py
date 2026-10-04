"""Integration checks that start an isolated local terminal gateway per test."""
import http.client
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import tempfile
import socket
import re
import struct
import signal
import unittest

HOST = '127.0.0.1:8035'
ORIGIN = 'http://' + HOST
CODE = '246813'

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

    def test_pairing_attempts_are_rate_limited(self):
        headers = {'Origin': ORIGIN, 'X-PS5-Client': '1'}
        for _ in range(5):
            self.assertEqual(self.request('POST', '/api/pair', '000000', headers)[0], 401)
        self.assertEqual(self.request('POST', '/api/pair', CODE, headers)[0], 429)

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

    def test_console_pairing_panel_renews_code_and_preserves_session(self):
        base = {'Origin': ORIGIN, 'X-PS5-Client': '1'}
        _, response, _ = self.request('POST', '/api/pair-local', '', base)
        headers = {**base, 'Cookie': response['Set-Cookie'].split(';')[0]}
        for invalid in ({}, base, {'Cookie': headers['Cookie'], 'Origin': ORIGIN},
                        {**headers, 'Origin': 'https://attacker.example'}):
            self.assertEqual(self.request('POST', '/api/pairing-code', '', invalid)[0], 403)
        self.assertEqual(self.request('POST', '/api/pairing-code', 'unexpected', headers)[0], 403)
        self.assertEqual(self.request('GET', '/api/pairing-code', headers=headers)[0], 404)
        status, _, body = self.request('POST', '/api/pairing-code', '', headers)
        self.assertEqual(status, 200)
        code = json.loads(body)
        self.assertRegex(code['code'], r'^\d{6}$')
        self.assertEqual(code['expires_in'], 900)
        self.assertEqual(self.request('POST', '/api/pair', code['code'], base)[0], 200)
        self.assertIn(b'"paired":true', self.request('GET', '/api/status', headers=headers)[2])
        if code['code'] != CODE:
            self.assertEqual(self.request('POST', '/api/pair', CODE, base)[0], 401)
        try:
            remote = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3,
                                                source_address=('127.0.0.2', 0))
            remote.request('POST', '/api/pairing-code', '',
                           {**headers, 'X-Forwarded-For': '127.0.0.1'})
            response = remote.getresponse()
            self.assertEqual(response.status, 403)
            response.read(); remote.close()
        except OSError as error:
            if not sys.platform.startswith('linux') and error.errno == 49:
                return  # macOS may not have the second loopback address.
            raise

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

    def test_restart_requires_pairing_origin_client_and_empty_body(self):
        path = '/api/cli/codex/restart'
        _, result, _ = self.request('POST', '/api/pair', CODE, {'Origin': ORIGIN, 'X-PS5-Client': '1'})
        cookie = result['Set-Cookie'].split(';')[0]
        good = {'Origin': ORIGIN, 'X-PS5-Client': '1', 'Cookie': cookie}
        for headers in ({}, {'Origin': ORIGIN, 'X-PS5-Client': '1'},
                        {**good, 'Origin': 'https://attacker.example'},
                        {'Origin': ORIGIN, 'Cookie': cookie}):
            with self.subTest(headers=headers):
                self.assertEqual(self.request('POST', path, '', headers)[0], 403)
        self.assertEqual(self.request('POST', path, 'unexpected', good)[0], 403)
        self.assertEqual(self.request('GET', path, headers=good)[0], 404)
        self.assertEqual(self.request('POST', '/api/cli/unknown/restart', '', good)[0], 404)

    def open_terminal(self, cookie):
        s = socket.create_connection(('127.0.0.1', self.port), timeout=4)
        s.sendall(('GET /terminal/codex?cols=100&rows=30 HTTP/1.1\r\n'
                   f'Host: {HOST}\r\nOrigin: {ORIGIN}\r\nCookie: {cookie}\r\n'
                   'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                   'Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n').encode())
        header = b''
        while not header.endswith(b'\r\n\r\n'): header += s.recv(1)
        self.assertIn(b'101', header.split(b'\r\n')[0])
        self.addCleanup(s.close)
        return s

    @staticmethod
    def terminal_send(s, text):
        data, mask = text.encode(), b'test'
        size = bytes([0x80 | len(data)]) if len(data) < 126 else b'\xfe' + struct.pack('!H', len(data))
        s.sendall(b'\x82' + size + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def terminal_until(self, s, pattern):
        def exact(size):
            result = b''
            while len(result) < size:
                data = s.recv(size-len(result))
                self.assertTrue(data, 'terminal closed before expected output')
                result += data
            return result
        output = b''
        for _ in range(100):
            header = exact(2)
            size = header[1] & 127
            if size == 126: size = struct.unpack('!H', exact(2))[0]
            elif size == 127: size = struct.unpack('!Q', exact(8))[0]
            output += exact(size)
            match = re.search(pattern, output)
            if match: return match
        self.fail('expected terminal output missing')

    def test_restart_replaces_hung_cli_preserves_files_and_pairing(self):
        _, result, _ = self.request('POST', '/api/pair', CODE, {'Origin': ORIGIN, 'X-PS5-Client': '1'})
        cookie = result['Set-Cookie'].split(';')[0]
        first = self.open_terminal(cookie)
        self.terminal_send(first, 'export PS5_RESTART_TEST=old; printf saved > "$HOME/saved-state"; printf "OLD_PID=%s\\n" "$$"\r')
        old_pid = self.terminal_until(first, rb'OLD_PID=(\d+)').group(1)
        self.terminal_send(first, 'trap "" TERM; printf "HANG_READY\\n"; while :; do sleep 1; done\r')
        self.terminal_until(first, rb'\r?\nHANG_READY\r?\n')
        headers = {'Origin': ORIGIN, 'X-PS5-Client': '1', 'Cookie': cookie}
        self.assertEqual(self.request('POST', '/api/cli/codex/restart', '', headers)[0], 200)
        self.assertIn(b'"paired":true', self.request('GET', '/api/status', headers=headers)[2])
        self.assertEqual((Path(self.temp.name)/'saved-state').read_text(), 'saved')
        second = self.open_terminal(cookie)
        self.terminal_send(second, 'printf "NEW_PID=%s VALUE=%s\\n" "$$" "${PS5_RESTART_TEST-unset}"\r')
        match = self.terminal_until(second, rb'NEW_PID=(\d+) VALUE=(\w+)')
        self.assertNotEqual(match.group(1), old_pid)
        self.assertEqual(match.group(2), b'unset')

    def test_cli_exit_sends_normal_websocket_close(self):
        _, result, _ = self.request('POST', '/api/pair', CODE,
                                    {'Origin': ORIGIN, 'X-PS5-Client': '1'})
        connection = self.open_terminal(result['Set-Cookie'].split(';')[0])
        self.terminal_send(connection, 'exit\r')

        def exact(size):
            data = b''
            while len(data) < size:
                chunk = connection.recv(size - len(data))
                self.assertTrue(chunk, 'CLI exit must send a close frame before EOF')
                data += chunk
            return data

        for _ in range(100):
            header = exact(2)
            size = header[1] & 127
            if size == 126: size = struct.unpack('!H', exact(2))[0]
            elif size == 127: size = struct.unpack('!Q', exact(8))[0]
            payload = exact(size)
            if header[0] & 15 == 8:
                self.assertEqual(payload[:2], struct.pack('!H', 1000))
                return
        self.fail('CLI exit did not send a normal close frame')

    def test_listener_recovers_without_losing_pairing_or_cli(self):
        _, result, _ = self.request('POST', '/api/pair', CODE,
                                    {'Origin': ORIGIN, 'X-PS5-Client': '1'})
        cookie = result['Set-Cookie'].split(';')[0]
        first = self.open_terminal(cookie)
        self.terminal_send(first, 'export RECOVERY_VALUE=retained; printf "BEFORE=%s\\n" "$$"\r')
        pid = self.terminal_until(first, rb'BEFORE=(\d+)').group(1)
        self.server.send_signal(signal.SIGUSR1)
        # Fault injection disables the listener. Recovery closes only the relay.
        first.settimeout(5)
        while first.recv(4096):
            pass
        first.close()
        deadline = time.monotonic() + 5
        while True:
            try:
                status, _, data = self.request('GET', '/api/status', headers={'Cookie': cookie})
                if status == 200:
                    self.assertTrue(json.loads(data)['paired'])
                    break
            except OSError:
                pass
            if time.monotonic() > deadline:
                self.fail('Listener did not recover')
            time.sleep(.05)
        second = self.open_terminal(cookie)
        self.terminal_send(second, 'printf "AFTER=%s VALUE=%s\\n" "$$" "$RECOVERY_VALUE"\r')
        restored = self.terminal_until(second, rb'AFTER=(\d+) VALUE=(\w+)')
        self.assertEqual(restored.group(1), pid)
        self.assertEqual(restored.group(2), b'retained')

if __name__ == '__main__':
    unittest.main()

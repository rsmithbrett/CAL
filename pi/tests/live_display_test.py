"""Real SDL executable + isolated loopback service; no production identities."""
import http.server
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import threading

secret = 'a' * 43
class Service(http.server.BaseHTTPRequestHandler):
    policy = {'cards': [{'id': 'forecast', 'kind': 'list', 'dwellSeconds': 2}, {'id': 'future-card'}]}
    revoked = False
    location = None
    scripted = False
    cycle = 0
    def log_message(self, *args):
        pass
    def reply(self, status, body):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(body).encode())
    def authorized(self):
        return not self.revoked and self.headers.get('X-Device-Secret') == secret
    def do_POST(self):
        assert self.path == '/api/checkin'
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert request['firmwareVersion'] == 'test'
        if self.scripted:
            type(self).cycle += 1
            if self.cycle == 5:
                self.reply(401, {})
                return
            if self.cycle == 3:
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"acknowledged":true,"cardPolicy":{"cards":[],"cards":[]}}')
                return
            policy = None if self.cycle == 2 else {'cards': [{'id': 'forecast', 'location': 'target' if self.cycle == 6 else 'home'}]}
            self.reply(200, {'acknowledged': True, 'cardPolicy': policy})
        else:
            self.reply(200 if self.authorized() else 401, {'acknowledged': True, 'cardPolicy': self.policy})
    def do_GET(self):
        assert self.path in ('/api/myweather/forecast?location=home', '/api/myweather/forecast?location=target')
        if self.scripted and self.cycle in (2,4):
            self.reply(500 if self.cycle == 2 else 403, {})
            return
        if not self.authorized():
            self.reply(401, {})
            return
        type(self).location = self.path.split('=')[1]
        self.reply(200, {'requestedLocation': self.location, 'city': 'Annapolis' if self.location == 'home' else 'Baltimore',
                         'state': 'MD', 'periods': [{'name': 'Today', 'temperature': 72 if self.location == 'home' else 65,
                                                   'temperatureUnit': 'F', 'shortForecast': 'Sunny'}]})

with tempfile.TemporaryDirectory() as directory:
    directory = pathlib.Path(directory)
    credentials = directory / 'credentials'
    credentials.write_text('installationId=00000000-0000-0000-0000-000000000001\ndeviceSecret=' + secret + '\n')
    credentials.chmod(0o600)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Service)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f'http://127.0.0.1:{server.server_port}'
    environment = dict(os.environ, SDL_VIDEODRIVER='dummy')
    def run(binary, name, code=0):
        output = directory / name
        result = subprocess.run([binary, '--server', origin, '--credentials', str(credentials), '--version', 'test',
                                 '--width', '320', '--height', '240', '--once', str(output)], env=environment,
                                capture_output=True, timeout=25)
        assert result.returncode == code, result.stderr
        assert secret.encode() not in result.stdout + result.stderr
        if code:
            assert not output.exists()
            return
        data = output.read_bytes()
        assert data[:2] == b'BM' and struct.unpack_from('<ii',data,18) == (320,240)
        evidence = os.environ.get('PI_TEST_FRAME_DIR')
        if evidence:
            destination = pathlib.Path(evidence)
            destination.mkdir(parents=True, exist_ok=True)
            (destination / name).write_bytes(data)
        return data
    home = run(sys.argv[1], 'home.bmp')
    assert Service.location == 'home'
    Service.policy = {'cards': [{'id': 'forecast5', 'location': 'target'}]}
    target = run(sys.argv[1], 'target.bmp')
    assert Service.location == 'target' and home != target
    Service.policy = {'cards': []}
    Service.location = None
    empty = run(sys.argv[1], 'empty.bmp')
    assert Service.location is None and empty != target
    Service.revoked = True
    run(sys.argv[1], 'revoked.bmp', 1)
    run(sys.argv[2], 'plaintext.bmp', 1) # release binary rejects plaintext
    Service.revoked = False
    Service.scripted = True
    result = subprocess.run([sys.argv[3], origin, str(credentials)], capture_output=True, timeout=25)
    assert result.returncode == 0, result.stderr
    assert secret.encode() not in result.stdout + result.stderr
    print(result.stdout.decode().strip())
    server.shutdown()
print('SDL loopback: authenticated policy/provider, target replacement, empty policy, revocation and HTTPS-only passed')

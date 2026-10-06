"""Exercise the native transport with isolated credentials and a loopback fake.

This checks the client wire behavior, not the server contract or Bolt 1 exit gate.
"""

import base64
import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import uuid


def main():
    test_binary, production_binary = map(str, sys.argv[1:3])
    secrets = [base64.urlsafe_b64encode(os.urandom(32)).decode().rstrip("=") for _ in range(2)]
    active = set(secrets)
    observed = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length))
            secret = self.headers.get("X-Device-Secret")
            observed.append((self.path, secret, payload))
            status = 200 if secret in active else 401
            body = json.dumps({"cardPolicy": {"cards": [{"cardId": str(secrets.index(secret))}]}}) if status == 200 else "{}"
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body.encode())

        def log_message(self, *_args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    origin = f"http://127.0.0.1:{server.server_port}"
    try:
        with tempfile.TemporaryDirectory() as directory:
            files = []
            for index, secret in enumerate(secrets):
                path = pathlib.Path(directory) / f"pi-{index}.credential"
                path.write_text(f"installationId={uuid.uuid4()}\ndeviceSecret={secret}\n")
                path.chmod(0o600)
                files.append(path)

            def run(binary, path):
                return subprocess.run([binary, origin, str(path), "pi-test"],
                                      capture_output=True, text=True, timeout=10)

            # The production binary cannot be switched to plaintext by an argument.
            denied = run(production_binary, files[0])
            assert denied.returncode != 0 and not observed
            for path in files:
                result = run(test_binary, path)
                assert result.returncode == 0 and "HTTP 200" in result.stdout
                assert all(secret not in result.stdout + result.stderr for secret in secrets)

            active.remove(secrets[0])
            revoked = run(test_binary, files[0])
            still_active = run(test_binary, files[1])
            assert revoked.returncode != 0 and "HTTP 401" in revoked.stdout
            assert still_active.returncode == 0 and "HTTP 200" in still_active.stdout
            assert [item[1] for item in observed] == [secrets[0], secrets[1], secrets[0], secrets[1]]
            assert all(path == "/api/checkin" and "deviceUtcTimestamp" in payload
                       and payload["firmwareVersion"] == "pi-test"
                       for path, _, payload in observed)
            print("native transport: two private identities, isolated revocation, TLS-only production binary passed")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()

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

from checkin_live_test import verify_pair


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
            body = json.dumps({"acknowledged": True, "cardPolicy": {"cards": [{"id": str(secrets.index(secret))}]}}) if status == 200 else "{}"
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
            policies = [{"cards": [{"id": str(index)}]} for index in range(2)]
            verify_pair(test_binary, origin, files, policies)

            active.remove(secrets[0])
            verify_pair(test_binary, origin, files, policies, revoked_first=True)
            assert [item[1] for item in observed] == [secrets[0], secrets[1], secrets[0], secrets[1]]
            assert all(path == "/api/checkin" and "deviceUtcTimestamp" in payload
                       and payload["firmwareVersion"] == "pi-live-test"
                       for path, _, payload in observed)
            # Capturing policy evidence cannot overwrite an existing credential.
            original = files[1].read_bytes()
            refused = subprocess.run([test_binary, origin, str(files[1]), "pi-test", "--response-file", str(files[1])],
                                     capture_output=True, text=True, timeout=10)
            assert refused.returncode != 0 and files[1].read_bytes() == original
            assert all(secret not in refused.stdout + refused.stderr for secret in secrets)
            active.add(secrets[0])
            try:
                verify_pair(test_binary, origin, files,
                            [{"cards": [{"id": "unexpected"}]}, policies[1]])
            except RuntimeError:
                pass
            else:
                raise AssertionError("Unexpected native policy was accepted")
            print("native transport: two private identities, isolated revocation, TLS-only production binary passed")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()

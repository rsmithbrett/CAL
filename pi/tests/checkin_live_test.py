"""Verify policy bytes received by two native Pi probes from a real HTTPS server.

Provision/activate two dedicated test devices and set distinct policies first.
Run once, revoke only the first test device, then rerun with --revoked-first.
Credential files are never printed, and temporary policy responses are private.
This host runner does not by itself prove ARMv6 or physical-device acceptance.
"""

import argparse
import json
import pathlib
import subprocess
import tempfile


def contains(actual, expected):
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(
            key in actual and contains(actual[key], value) for key, value in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(
            contains(a, e) for a, e in zip(actual, expected))
    return type(actual) is type(expected) and actual == expected


def verify_pair(binary, origin, credentials, expected_policies, revoked_first=False):
    if expected_policies[0] == expected_policies[1]:
        raise RuntimeError("Expected test policies must differ")
    with tempfile.TemporaryDirectory() as directory:
        for index, credential in enumerate(credentials):
            output = pathlib.Path(directory) / f"response-{index}.json"
            result = subprocess.run(
                [str(binary), origin, str(credential), "pi-live-test", "--response-file", str(output)],
                capture_output=True, text=True, timeout=25)
            if index == 0 and revoked_first:
                if result.returncode == 0 or "check-in HTTP 401," not in result.stdout or output.exists():
                    raise RuntimeError("First test Pi was not refused after revocation")
                continue
            if result.returncode != 0 or not output.is_file():
                raise RuntimeError("Native test Pi did not complete check-in")
            if output.stat().st_mode & 0o077:
                raise RuntimeError("Native policy evidence was not private")
            response = json.loads(output.read_text())
            if response.get("acknowledged") is not True:
                raise RuntimeError("Server did not acknowledge the test Pi")
            if not contains(response.get("cardPolicy"), expected_policies[index]):
                raise RuntimeError("Test Pi received an unexpected card policy")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("origin")
    parser.add_argument("credential_a", type=pathlib.Path)
    parser.add_argument("credential_b", type=pathlib.Path)
    parser.add_argument("policy_a", type=pathlib.Path, help="Expected cardPolicy JSON or a subset")
    parser.add_argument("policy_b", type=pathlib.Path)
    parser.add_argument("--revoked-first", action="store_true")
    args = parser.parse_args()
    if not args.origin.startswith("https://"):
        parser.error("Live tests require HTTPS")
    try:
        policies = [json.loads(path.read_text()) for path in [args.policy_a, args.policy_b]]
        verify_pair(args.binary.resolve(), args.origin,
                    [args.credential_a, args.credential_b], policies, args.revoked_first)
    except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired):
        # Avoid leaking credentials, returned content or fixture values through errors.
        raise SystemExit("Live Pi policy/identity check failed; inspect the private test setup")
    print("Native Pi policy check passed" + ("; first revoked, second still correct" if args.revoked_first else "; both correct"))


if __name__ == "__main__":
    main()

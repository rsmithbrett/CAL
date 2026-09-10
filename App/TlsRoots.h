#pragma once

/// The trust anchors this firmware validates the service's certificate against.
///
/// **Why these two certificates instead of the ESP32 core's root bundle.**
///
/// The core embeds a snapshot of Mozilla's full root program - roughly 150
/// certificates - and Tls.cpp used to hand all of it to
/// NetworkClientSecure::setCACertBundle(). That failed in the field, and the
/// failure is what this file exists to fix:
///
///   E esp-x509-crt-bundle: PK verify failed with error 0x10
///   E esp-x509-crt-bundle: Certificate matched but signature verification failed
///   E esp-x509-crt-bundle: Failed to verify certificate
///
/// 0x10 is mbedTLS's MBEDTLS_ERR_MPI_ALLOC_FAILED (-0x0010, see bignum.h), so
/// the verification did not fail on trust - it failed on memory, part way
/// through the bignum arithmetic. The service's certificate chain is
/// api.discoveraroundme.com <- YE2 <- Root YE <- ISRG Root X2, four levels
/// deep and ECDSA P-384 at every link. Verifying that in software on this chip
/// means a great many small MPI allocations, and it is the TOTAL free 8-bit
/// heap that decides whether they all succeed, not the largest contiguous
/// block. Measured on two real devices at the same moment:
///
///   device 7:  free8=10,948  largest8=34,804  -> verification FAILS
///   device 17: free8=20,764  largest8=14,324  -> verification SUCCEEDS
///
/// The device with the far bigger contiguous block is the one that fails,
/// which is what makes this a working-set problem rather than a fragmentation
/// one. It also explains why the symptom is intermittent: free8 is plentiful
/// just after boot and falls as cards render, so the first check-in after a
/// restart succeeds and later ones stop working.
///
/// Pinning the two roots directly attacks that cost. mbedTLS parses the trust
/// material it is given, so two certificates cost dramatically less to hold
/// and search than 150, and verification stops depending on how fragmented the
/// heap happens to be when a check-in lands.
///
/// **The trade this makes, stated plainly.** The core bundle trusts
/// essentially every public CA, so the service could move to any issuer and
/// devices would follow. This does not: if the service ever stops chaining to
/// an ISRG root, every device pinned this way loses its connection to the
/// server - including the OTA path that would deliver the fix. That is a real
/// and deliberate risk, accepted because the alternative measured here is a
/// fleet that intermittently cannot talk to the server at all.
///
/// Both ISRG roots are included rather than only the one currently in use.
/// X2 is the ECDSA root the live chain actually terminates at; X1 is the older
/// RSA root, included so a reissue onto Let's Encrypt's RSA hierarchy - which
/// needs no device change and could happen without warning - still validates.
///
/// **These bytes were not transcribed by hand.** They were exported from the
/// host's own trust store and then proven against the live service before
/// being embedded:
///
///   openssl verify -CAfile isrg_roots.pem -untrusted inter.pem leaf.pem
///   leaf.pem: OK
///
/// where inter.pem/leaf.pem came from `openssl s_client -showcerts` against
/// api.discoveraroundme.com. So the chain the server actually sends is known
/// to validate against exactly these two anchors and the intermediates the
/// server supplies. Fingerprints, for anyone checking them against ISRG's
/// published values:
///
///   ISRG Root X1  SHA-1  CA:BD:2A:79:A1:07:6A:31:F2:1D:25:36:35:CB:03:9D:43:29:A5:E8
///   ISRG Root X2  SHA-1  BD:B1:B9:3C:D5:97:8D:45:C6:26:14:55:F8:DB:95:C7:5A:D1:53:AF
///
/// X1 expires 2035-06-04 and X2 2040-09-17, so neither is a near-term
/// maintenance concern - but both are dates this firmware has an opinion about,
/// which the bundle version never did.
namespace TlsRoots {

/// Concatenated PEM, which is what mbedTLS expects for multiple anchors -
/// setCACert takes one NUL-terminated buffer and parses every certificate it
/// finds in it.
inline constexpr const char* kIsrgRoots =
    // ISRG Root X2 - ECDSA P-384. First because it is the anchor the live
    // chain terminates at, so the common case is found immediately.
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n"
    // ISRG Root X1 - RSA 4096. Not on the current chain; here so a move back
    // to the RSA hierarchy does not need a firmware release.
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

}  // namespace TlsRoots

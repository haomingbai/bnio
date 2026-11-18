// Embedded test PKI for this example: one self-signed CA and two leaf
// certificates whose subjectAltName covers a.example.test / b.example.test.
// The CA private key is deliberately absent. The leaf private keys below are
// throwaway test material and must never be reused outside this demo.
inline constexpr char k_ca_certificate_pem[] = R"pem(
-----BEGIN CERTIFICATE-----
MIIBrTCCAVOgAwIBAgIUcP2Hm1FAP+YqwFEod/Dkh/VsyY8wCgYIKoZIzj0EAwIw
MzESMBAGA1UECgwJYm5pbyBkZW1vMR0wGwYDVQQDDBRibmlvIGR5bmFtaWMgdGVz
dCBDQTAeFw0yNjExMTgxMzM5MDJaFw0zNjExMTUxMzM5MDJaMDMxEjAQBgNVBAoM
CWJuaW8gZGVtbzEdMBsGA1UEAwwUYm5pbyBkeW5hbWljIHRlc3QgQ0EwWTATBgcq
hkjOPQIBBggqhkjOPQMBBwNCAATI9S/so/BOfz6ZGrN0/j93jx5q56TyeL2jyTz9
gx97gIduHviTEs8ywmKQiYySf+ht/WBd7mguH1LA/w9GbLrco0UwQzAdBgNVHQ4E
FgQUI/tdYm+0+hgvx9VhczzHPP2j300wEgYDVR0TAQH/BAgwBgEB/wIBADAOBgNV
HQ8BAf8EBAMCAQYwCgYIKoZIzj0EAwIDSAAwRQIhAKObKf/k1MPsUYXfFcsarz5Z
/obJMrN/Siq3YDO9u9YFAiA6EnjIAUUyaRCqD2wSRuW7rRPxB/mmC+XzTOuFJJe1
5Q==
-----END CERTIFICATE-----
)pem";

inline constexpr char k_a_certificate_pem[] = R"pem(
-----BEGIN CERTIFICATE-----
MIIBsTCCAVigAwIBAgIUNkVEZX3WI1rA5KQW5h6V2sK7ZNIwCgYIKoZIzj0EAwIw
MzESMBAGA1UECgwJYm5pbyBkZW1vMR0wGwYDVQQDDBRibmlvIGR5bmFtaWMgdGVz
dCBDQTAeFw0yNjExMTgxMzM5MDJaFw0zNjExMTUxMzM5MDJaMC0xEjAQBgNVBAoM
CWJuaW8gZGVtbzEXMBUGA1UEAwwOYS5leGFtcGxlLnRlc3QwWTATBgcqhkjOPQIB
BggqhkjOPQMBBwNCAARrQq9pzFfIPMWtsZhsvcII8Hey1FqIXw/n6NUFIFVf6xDq
6F3wZa3JgnaDuF1Hc2GWVbudbGT2kd2OLUVDgCx6o1AwTjAMBgNVHRMBAf8EAjAA
MA4GA1UdDwEB/wQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDATAZBgNVHREEEjAQ
gg5hLmV4YW1wbGUudGVzdDAKBggqhkjOPQQDAgNHADBEAiAz6QSfEdAZivaPL6d0
M0cGjQk8nWVomioxIs4YoE1YFQIgDmgWW/J/qvtmS/RFpJKdXT3lSdj89dfoJXtz
1iLenQ4=
-----END CERTIFICATE-----
)pem";

inline constexpr char k_a_private_key_pem[] = R"pem(
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEICSMTHY/r09MizBWsr4kp+i4/wTW8mX8L0Hj7JBUfcWDoAoGCCqGSM49
AwEHoUQDQgAEa0KvacxXyDzFrbGYbL3CCPB3stRaiF8P5+jVBSBVX+sQ6uhd8GWt
yYJ2g7hdR3NhllW7nWxk9pHdji1FQ4Aseg==
-----END EC PRIVATE KEY-----
)pem";

inline constexpr char k_b_certificate_pem[] = R"pem(
-----BEGIN CERTIFICATE-----
MIIBsTCCAVigAwIBAgIUNkVEZX3WI1rA5KQW5h6V2sK7ZNMwCgYIKoZIzj0EAwIw
MzESMBAGA1UECgwJYm5pbyBkZW1vMR0wGwYDVQQDDBRibmlvIGR5bmFtaWMgdGVz
dCBDQTAeFw0yNjExMTgxMzM5MDJaFw0zNjExMTUxMzM5MDJaMC0xEjAQBgNVBAoM
CWJuaW8gZGVtbzEXMBUGA1UEAwwOYi5leGFtcGxlLnRlc3QwWTATBgcqhkjOPQIB
BggqhkjOPQMBBwNCAAR+NjUuumf21NYIOEaJjLGtiLN3MvpJSrS8TnAGe3uFIHod
d2xs7VFNvxVSYFttyofqW5J42ynRNdrbNM7QcA7Qo1AwTjAMBgNVHRMBAf8EAjAA
MA4GA1UdDwEB/wQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDATAZBgNVHREEEjAQ
gg5iLmV4YW1wbGUudGVzdDAKBggqhkjOPQQDAgNHADBEAiBZJwMwqT8wpRmFYRNs
S6loOJDFLUshzeUCDsgiREK+7AIgO4IsYFRGwIbVc5ssvE98NoVU8JK6Ls42b6Kg
6Z627Ug=
-----END CERTIFICATE-----
)pem";

inline constexpr char k_b_private_key_pem[] = R"pem(
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEII6h5GJFFVWX6tT60YLpyXysmIQkizcF8RveEAZJ91qooAoGCCqGSM49
AwEHoUQDQgAEfjY1Lrpn9tTWCDhGiYyxrYizdzL6SUq0vE5wBnt7hSB6HXdsbO1R
Tb8VUmBbbcqH6luSeNsp0TXa2zTO0HAO0A==
-----END EC PRIVATE KEY-----
)pem";

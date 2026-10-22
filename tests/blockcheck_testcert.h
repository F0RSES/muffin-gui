#pragma once

// A throwaway self-signed certificate + key pair, generated once with
//   openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=blockcheck-test.invalid
//       -addext subjectAltName=DNS:localhost,IP:127.0.0.1
// and pasted here verbatim. Its only job in life is to be REJECTED: coretest's TLS section stands up
// a QSslServer with it and asserts that BlockcheckEngine::checkHost() fails the handshake and names
// the certificate as the reason — the substitution shape the engine exists to catch. It is not a
// secret (it never was one), it is trusted by nothing, and its CN is a .invalid name that cannot
// resolve. If it ever expires (2036), regenerate with the command above and paste again.

#include <QByteArray>

namespace zaprettest {

inline const QByteArray &blockcheckTestCert()
{
    static const QByteArray pem = QByteArrayLiteral(R"PEM(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgIUNbOuzK64hbeD4j7QQps+B39+w/MwDQYJKoZIhvcNAQEL
BQAwIjEgMB4GA1UEAwwXYmxvY2tjaGVjay10ZXN0LmludmFsaWQwHhcNMjYxMDIx
MTkzNzAwWhcNMzYxMDE4MTkzNzAwWjAiMSAwHgYDVQQDDBdibG9ja2NoZWNrLXRl
c3QuaW52YWxpZDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBANdGr1NP
MfOyRJZsF1ZTO8D1s3DXWs4vtaLJ0BvpwdrUUr5tBj+6VZ8ZDv5D1rsr70I88o6+
px/flK+RU91LE2feRnX+9/t+saKv51Jl/Zf2e6W1nGNNRYcoGkHIqOO6ZcFWVfx7
vAVrr2HJvLYYxJX8eFCcXRquv/RKtoBfS2bPbdQy3yqXPKyGF8hWpUu8iIXA0z53
u3ah5bgk8UCM2a0HKBu/O3PqhwW74y3BHj0JiNhzSiaSk8gqfF04E8udzGPQlplK
Bp85gme8LFycpOqZWuLIZFCOxcwr7rSHuoCvGMI4zOBcgLRGgVy5nF/8fzERywNV
LfwhK+Tj4Q9bEL0CAwEAAaNvMG0wHQYDVR0OBBYEFPgkWyMevbzd8W+M4iESkyeV
AeU8MB8GA1UdIwQYMBaAFPgkWyMevbzd8W+M4iESkyeVAeU8MA8GA1UdEwEB/wQF
MAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMA0GCSqGSIb3DQEBCwUA
A4IBAQAEcSh7xTXUh8g9Fw4gJH6JI8SzBX/SuigXZEdfHEJwXPuEzND4p1yixlfN
38OYXLdcg1N5ouvFvbjswRBYITA0hScUfn81CVfXm0rGCmj1u1H7ZjR7u2SW4ZFE
KCR0PMEgvm8JfJJUIIeDNNUdcWweI5bWUywfk0+bZgL9bJ5TIoxWZt9181t6Mebm
s24US6e/k5WeSa3ALPzK7NFBn4hVgNLpQLrBM1VKHYp6hOFsUgnh21nB+CXdQTat
sooJ2DNzMxZi6C58M1EhsGqVkSeh+Y4x5KTG/ZzMRRhSsOk+ZPIyNeTmwWTlXZMr
p+4QLvj4+vII/SyTygxUIblqaJkR
-----END CERTIFICATE-----
)PEM");
    return pem;
}

inline const QByteArray &blockcheckTestKey()
{
    static const QByteArray pem = QByteArrayLiteral(R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDXRq9TTzHzskSW
bBdWUzvA9bNw11rOL7WiydAb6cHa1FK+bQY/ulWfGQ7+Q9a7K+9CPPKOvqcf35Sv
kVPdSxNn3kZ1/vf7frGir+dSZf2X9nultZxjTUWHKBpByKjjumXBVlX8e7wFa69h
yby2GMSV/HhQnF0arr/0SraAX0tmz23UMt8qlzyshhfIVqVLvIiFwNM+d7t2oeW4
JPFAjNmtBygbvztz6ocFu+MtwR49CYjYc0omkpPIKnxdOBPLncxj0JaZSgafOYJn
vCxcnKTqmVriyGRQjsXMK+60h7qArxjCOMzgXIC0RoFcuZxf/H8xEcsDVS38ISvk
4+EPWxC9AgMBAAECggEAEQvmVzT2siLkHnspr+rAWQu7rUxIQvpxKhZL9eHhOVhF
wxF0BLjXDdWYo9awWrrkY7zawiXjqvjz16Es4GGRBFEMCEzrywzhuQ2vxg0QcyeP
6/PBEfXs8wJgbeQbR4kbhJ83CNqtbUp1JUupt0unyW6oAk4Xa0T6Kzh9UhX5rQbL
nTgQ7MvQt5/m7k6+uJCGxMsOoA1p3ZKA/ko/Brw28BGoEtbDKwLu18NOh1f+d9jh
QW2uvsNL8PJh9O7lf1tu40Osh4ugtaom16m5cfe9I3x8yrNAq2NLpWjwMRvGCEWE
sCo3cymi5VwBvaAuxU+T4HbE/yB4JVrILZqpoQ/1+wKBgQD6IMGp/HBIufg/acix
NJRceAsbE38jJFGRL1RtYt2n501hBugtlJHZC+zj/T4ZDSJMy/Ri2yAP7M3wP2NF
cP7WuKBVvg9RJKLx0AMjkPjqalZW117gDY0fQGG90ZAo3DL7DDuaoXTC9t2Q2103
b+QNQAgIMyZh0s2/e9r94u7/SwKBgQDcVHjru/DDSTi/X4hNwgAfwSheAt0SnLww
FuYbMBQwmV9+/Dt3QNfJWDHjzcBSkdTg65682T7U2GkOra+W99ccNe1Yy6IXwVWx
7KqZkhYrdNw45I/zkZ267xzD1uhRolYiNzCkZvUOt0Xo84Pja9klG4EeJ01LfegH
lalyPsTDFwKBgQDqVrYaY/9uo40JHDqCixhUacM94s5CRkyFESqwMOgp3nKIbMgp
q//YsWuHTkNXhQ6VlJ+p3lHzmZB3fWusK2kWEnNk4WPISeK/WnsWX0RVmrq5MZ4Z
m5iTpuXryOKe5W599cAB5zJVVqPOizaSMaofH9ZmmEVlcPKCa6+IHYx8cQKBgQCQ
u+DPIh9veWsQs8XLJpSER5On5gOpVVJ92J0B4RxyLF5CAZlipjLaLdhgB+m9k9Se
h8k3zxHqKdshbFD7QAWLn5UxjlsFddhAT/JlfUSY+WQs71/spYxSnv5rW4IzFgu0
4EJQzN1P9G61LzpFK7RyHtY7o5+v5p6EVPkBdmA0swKBgQCMuewsvmIyBbwxGnXJ
G6xW09ZLnk8m1MxmZKFC5U+QNdsiydZrgaj1jeaPywnJeEY+R6724OSKy4bZdUP8
CQXsReg+4KSbhf3LJeh0SDT2/Ay2yv3+YWJrMjELERykJoNM5om6cGWExKzuWkJG
Ex/MXtsDbQ3ciy86al2h9KWjag==
-----END PRIVATE KEY-----
)PEM");
    return pem;
}

} // namespace zaprettest

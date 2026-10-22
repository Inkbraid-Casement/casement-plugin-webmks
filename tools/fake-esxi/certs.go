package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"fmt"
	"math/big"
	"net"
	"strings"
	"time"
)

// keyPair is a certificate with its key, and its DER encoding for
// fingerprints.
type keyPair struct {
	tls tls.Certificate
	der []byte
}

func fingerprint(der []byte, sha string) string {
	var sum []byte
	if sha == "sha1" {
		s := sha1.Sum(der)
		sum = s[:]
	} else {
		s := sha256.Sum256(der)
		sum = s[:]
	}
	parts := make([]string, len(sum))
	for i, b := range sum {
		parts[i] = fmt.Sprintf("%02X", b)
	}
	return strings.Join(parts, ":")
}

func newKey() *ecdsa.PrivateKey {
	k, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		panic(err)
	}
	return k
}

func serial() *big.Int {
	n, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 63))
	if err != nil {
		panic(err)
	}
	return n
}

// newCA makes a root certificate for the run.
func newCA() (*x509.Certificate, *ecdsa.PrivateKey, []byte) {
	key := newKey()
	tmpl := &x509.Certificate{
		SerialNumber:          serial(),
		Subject:               pkix.Name{CommonName: "fake-esxi test CA"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(48 * time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		panic(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		panic(err)
	}
	return cert, key, der
}

// newServerCert makes a server certificate for the addresses, signed by the
// CA, or self-signed when ca is nil (as ESXi's are by default).
func newServerCert(name string, ips []string, ca *x509.Certificate, caKey *ecdsa.PrivateKey) keyPair {
	key := newKey()
	tmpl := &x509.Certificate{
		SerialNumber: serial(),
		Subject:      pkix.Name{CommonName: name},
		DNSNames:     []string{name},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(48 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	for _, ip := range ips {
		tmpl.IPAddresses = append(tmpl.IPAddresses, net.ParseIP(ip))
	}
	parent, signer := tmpl, key
	if ca != nil {
		parent, signer = ca, caKey
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, parent, &key.PublicKey, signer)
	if err != nil {
		panic(err)
	}
	return keyPair{tls: tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, der: der}
}

/*
 Copyright (C) 2015-2020 IoT.bzh

 author: José Bollo <jose.bollo@iot.bzh>

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/


#include <openssl/x509.h>

#include "verbose.h"
#include "wgtpkg-certs.h"
#include "wgtpkg-base64.h"

struct x509l {
	unsigned count;
	X509 **certs;
};

static struct x509l certificates = { .count = 0, .certs = NULL };

static int add_certificate_x509(X509 *x)
{
	X509 **p = realloc(certificates.certs,
			(certificates.count + 1) * sizeof(X509*));
	if (!p) {
		ERROR("reallocation failed for certificate");
		return -1;
	}
	certificates.certs = p;
	p[certificates.count++] = x;
	return 0;
}

static int add_certificate_bin(const char *bin, size_t len)
{
	int rc;
	const char *b, *e;
	b = bin;
	e = bin + len;
	while (b < e) {
		X509 *x =  d2i_X509(NULL, (const unsigned char **)&b, e-b);
		if (x == NULL) {
			ERROR("d2i_X509 failed");
			return -1;
		}
		rc = add_certificate_x509(x);
		if (rc) {
			X509_free(x);
			return rc;
		}
	}
	return 0;
}

int add_certificate_b64(const char *b64)
{
	char *d;
	ssize_t l = base64dec(b64, &d);
	int rc;
	if (l < 0)
		rc = -1;
	else {
		rc = add_certificate_bin(d, (size_t)l);
		free(d);
	}
	return rc;
}

void clear_certificates()
{
	while(certificates.count)
		X509_free(certificates.certs[--certificates.count]);
}



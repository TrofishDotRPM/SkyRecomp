// SHA-256 and HMAC-SHA-256 against FIPS 180-4 and RFC 4231 test vectors:
//   cc -I runtime/hle tools/test_coop_sha256.c runtime/hle/coop_sha256.c -o /tmp/t && /tmp/t
#include <stdio.h>
#include <string.h>

#include "coop_sha256.h"

static int fails;
static void check(const char *what, const uint8_t got[32], const char *want_hex) {
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", got[i]);
    if (strcmp(hex, want_hex)) { printf("FAIL %s: %s\n", what, hex); fails++; }
}

int main(void) {
    uint8_t out[32];
    coop_sha256("", 0, out);
    check("sha empty", out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    coop_sha256("abc", 3, out);
    check("sha abc", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // (two blocks)
    coop_sha256(two, strlen(two), out);
    check("sha 448 bits", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    static char million[1000000];
    memset(million, 'a', sizeof million);
    coop_sha256(million, sizeof million, out);
    check("sha million a", out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // RFC 4231 cases 1, 2 and 6 (a key longer than a block)
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof key1);
    coop_hmac_sha256(key1, sizeof key1, "Hi There", 8, out);
    check("hmac 1", out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    coop_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    check("hmac 2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof key6);
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    coop_hmac_sha256(key6, sizeof key6, m6, strlen(m6), out);
    check("hmac 6", out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}

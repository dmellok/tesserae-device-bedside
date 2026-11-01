/*
 * test_misc.c: SHA-256 (it verifies every update before the panel restarts
 * into it) and release version ordering (which release counts as newer).
 */
#include "test.h"
#include "sha256.h"
#include "ota.h"

static void hex(const uint8_t *d, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]); }

static void sha256_known_answers(void)
{
    struct { const char *in, *out; } V[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++) {
        sha256_t s; uint8_t d[32]; char h[65];
        sha256_init(&s); sha256_update(&s, V[i].in, strlen(V[i].in)); sha256_final(&s, d); hex(d, h);
        CHECK_STR(h, V[i].out);
    }
}

static void sha256_in_pieces_matches_whole(void)
{
    /* The updater hashes the image in 4 KB blocks as it reads flash back. */
    static uint8_t buf[100003];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 31 + 7);
    sha256_t a, b; uint8_t da[32], db[32];
    sha256_init(&a); sha256_update(&a, buf, sizeof buf); sha256_final(&a, da);
    sha256_init(&b);
    for (size_t o = 0; o < sizeof buf; o += 4096) sha256_update(&b, buf + o, sizeof buf - o < 4096 ? sizeof buf - o : 4096);
    sha256_final(&b, db);
    CHECK(memcmp(da, db, 32) == 0);
}

static void sha256_million_a(void)
{
    sha256_t s; uint8_t d[32]; char h[65];
    static char a[1000000];
    memset(a, 'a', sizeof a);
    sha256_init(&s); sha256_update(&s, a, sizeof a); sha256_final(&s, d); hex(d, h);
    CHECK_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void versions_order_numerically(void)
{
    CHECK(ota_version_cmp("0.2.1", "0.2.0") > 0);
    CHECK(ota_version_cmp("0.10.0", "0.9.9") > 0);              /* not alphabetical */
    CHECK(ota_version_cmp("1.0.0", "0.99.99") > 0);
    CHECK(ota_version_cmp("0.2.0", "0.2.0") == 0);
    CHECK(ota_version_cmp("0.2.0", "0.2.1") < 0);
    CHECK(ota_version_cmp("v0.3.0", "0.2.9") > 0);              /* a tag with its v */
    CHECK(ota_version_cmp("0.3", "0.2.9") > 0);                 /* missing parts count as 0 */
    CHECK(ota_version_cmp("dev-abc1234", "0.0.1") < 0);         /* a dev build: any release is newer */
}

int main(void)
{
    RUN(sha256_known_answers);
    RUN(sha256_in_pieces_matches_whole);
    RUN(sha256_million_a);
    RUN(versions_order_numerically);
    return test_summary("misc");
}

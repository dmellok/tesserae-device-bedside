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

/* ---- chunked transfer decoding: GitHub answers the release list chunked now and then ---- */
#include "http_chunked.h"
#include <string.h>

static const char CHUNKED[] = "4\r\nWiki\r\n5\r\npedia\r\nE;ext=1\r\n in\r\n\r\nchunks.\r\n0\r\nTrailer: x\r\n\r\n";
static const char PLAIN[]   = "Wikipedia in\r\n\r\nchunks.";

static void chunked_whole_body(void)
{
    chunked_t c; uint8_t out[64]; chunked_init(&c);
    size_t n = chunked_feed(&c, (const uint8_t *)CHUNKED, strlen(CHUNKED), out);
    CHECK_EQ(n, strlen(PLAIN)); CHECK(memcmp(out, PLAIN, n) == 0); CHECK(c.done); CHECK(!c.bad);
}

static void chunked_any_split_gives_the_same_payload(void)
{
    /* TLS records split the body anywhere, so every split must decode alike. */
    size_t len = strlen(CHUNKED);
    for (size_t step = 1; step <= 7; step++) {
        chunked_t c; uint8_t out[64]; size_t n = 0; chunked_init(&c);
        for (size_t i = 0; i < len; i += step) {
            size_t k = len - i < step ? len - i : step;
            n += chunked_feed(&c, (const uint8_t *)CHUNKED + i, k, out + n);
        }
        CHECK_EQ(n, strlen(PLAIN)); CHECK(memcmp(out, PLAIN, n) == 0); CHECK(c.done);
    }
}

static void chunked_decodes_in_place(void)
{
    /* The REST client decodes a small body where it landed, in its header buffer. */
    char buf[80]; strcpy(buf, CHUNKED);
    chunked_t c; chunked_init(&c);
    size_t n = chunked_feed(&c, (const uint8_t *)buf, strlen(CHUNKED), (uint8_t *)buf);
    buf[n] = 0; CHECK_STR(buf, PLAIN); CHECK(c.done);
}

static void chunked_rejects_bad_framing(void)
{
    chunked_t c; uint8_t out[16]; chunked_init(&c);
    chunked_feed(&c, (const uint8_t *)"zz\r\nabc", 7, out);
    CHECK(c.bad);
    chunked_init(&c);
    size_t n = chunked_feed(&c, (const uint8_t *)"3\r\nabcX", 7, out);   /* no CRLF after the data */
    CHECK_EQ(n, 3); CHECK(c.bad); CHECK(!c.done);
}

int main(void)
{
    RUN(sha256_known_answers);
    RUN(sha256_in_pieces_matches_whole);
    RUN(sha256_million_a);
    RUN(versions_order_numerically);
    RUN(chunked_whole_body);
    RUN(chunked_any_split_gives_the_same_payload);
    RUN(chunked_decodes_in_place);
    RUN(chunked_rejects_bad_framing);
    return test_summary("misc");
}

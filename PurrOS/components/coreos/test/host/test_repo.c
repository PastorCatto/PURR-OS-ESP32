#include <stdio.h>
#include <string.h>

#include "purr_repo.h"
#include "purr_util.h"
#include "testkit.h"

/* An index with one entry whose hash is of `body`. */
static void hex_of(const uint8_t *d, size_t n, char out[65])
{
    uint8_t h[32];
    purr_sha256(d, n, h);
    for (int i = 0; i < 32; i++) {
        snprintf(out + i * 2, 3, "%02x", h[i]);
    }
}

static const uint8_t BODY[] = "pretend this is a .cat package";

static int parse(const char *json, purr_repo_index_t *idx, const purr_repo_kind_t *kind)
{
    char err[80];
    return purr_repo_index_parse(json, strlen(json), idx, kind, err, sizeof(err));
}

static void test_parse_good(void)
{
    char hex[65], doc[2048];
    hex_of(BODY, sizeof(BODY) - 1, hex);
    snprintf(doc, sizeof(doc),
             "{\"format\":1,\"repo\":{\"id\":\"mac-1\",\"name\":\"Zander's SDK\",\"kind\":\"sdk\"},"
             "\"apps\":[{\"name\":\"clock\",\"title\":\"Clock\",\"version\":\"1.2.0\",\"size\":%u,"
             "\"sha256\":\"%s\",\"chip\":\"esp32s3\",\"abi\":2,\"signed\":true,\"key_id\":2,"
             "\"desc\":\"tells the time\",\"url\":\"apps/clock/1.2.0/package.cat\",\"updated\":1793000000},"
             "{\"name\":\"rogue\",\"version\":\"0.1\",\"size\":%u,\"sha256\":\"%s\","
             "\"url\":\"apps/rogue/0.1/package.cat\",\"signed\":false}]}",
             (unsigned)(sizeof(BODY) - 1), hex, (unsigned)(sizeof(BODY) - 1), hex);
    purr_repo_index_t idx;
    CHECK_EQ(parse(doc, &idx, NULL), 0);
    CHECK_EQ(idx.format, 1);
    CHECK_EQ(idx.kind, PURR_REPO_SDK);
    CHECK(strcmp(idx.id, "mac-1") == 0);
    CHECK(strcmp(idx.name, "Zander's SDK") == 0);
    CHECK_EQ(idx.count, 2);
    CHECK_EQ(idx.dropped, 0);
    const purr_repo_app_t *c = purr_repo_index_find(&idx, "clock");
    CHECK(c != NULL);
    if (c) {
        CHECK(strcmp(c->title, "Clock") == 0);
        CHECK(strcmp(c->version, "1.2.0") == 0);
        CHECK(strcmp(c->chip, "esp32s3") == 0);
        CHECK_EQ(c->size, sizeof(BODY) - 1);
        CHECK_EQ(c->abi, 2);
        CHECK_EQ(c->key_id, 2);
        CHECK_EQ(c->signed_claim, 1);
        CHECK_EQ(c->updated, 1793000000u);
        CHECK(strcmp(c->desc, "tells the time") == 0);
        CHECK_EQ(purr_repo_check_package(c, BODY, sizeof(BODY) - 1), PURR_REPO_PKG_OK);
    }
    const purr_repo_app_t *r = purr_repo_index_find(&idx, "rogue");
    CHECK(r != NULL);
    if (r) {
        CHECK(strcmp(r->title, "rogue") == 0);       /* title defaults to the name */
        CHECK(strcmp(r->chip, "any") == 0);          /* chip defaults to any */
        CHECK_EQ(r->signed_claim, 0);
    }
    CHECK(purr_repo_index_find(&idx, "nope") == NULL);
}

static void test_unknown_fields_and_escapes(void)
{
    char hex[65], doc[2048];
    hex_of(BODY, sizeof(BODY) - 1, hex);
    snprintf(doc, sizeof(doc),
             "{ \"extra\": {\"deep\": [1, 2, {\"x\": null}], \"t\": true}, \"format\": 1,\n"
             " \"repo\": {\"kind\": \"sdk\", \"future\": [\"a\",\"b\"]},\n"
             " \"apps\": [ {\"name\":\"a\", \"version\":\"1.0\", \"size\": %u, \"sha256\":\"%s\","
             "  \"url\":\"p/a.cat\", \"desc\":\"quote \\\" slash \\\\ tab \\t caf\\u00e9\","
             "  \"someday\": {\"x\": [1,2,3]}, \"ratio\": 1.5e3 } ] }",
             (unsigned)(sizeof(BODY) - 1), hex);
    purr_repo_index_t idx;
    CHECK_EQ(parse(doc, &idx, NULL), 0);
    CHECK_EQ(idx.count, 1);
    CHECK(strstr(idx.apps[0].desc, "quote \" slash \\ tab \t caf?") != NULL);
}

static void test_malformed_documents(void)
{
    purr_repo_index_t idx;
    CHECK_EQ(parse("", &idx, NULL), -1);
    CHECK_EQ(parse("[]", &idx, NULL), -1);
    CHECK_EQ(parse("{", &idx, NULL), -1);
    CHECK_EQ(parse("{\"format\":1,\"apps\":[", &idx, NULL), -1);
    CHECK_EQ(parse("{\"format\":1,\"apps\":[{\"name\":\"a\"", &idx, NULL), -1);
    CHECK_EQ(parse("{\"format\":3,\"apps\":[]}", &idx, NULL), -1);          /* a format we do not know */
    CHECK_EQ(parse("{\"apps\":[]}", &idx, NULL), -1);                       /* no format at all */
    CHECK_EQ(parse("{\"format\":1,\"repo\":{\"kind\":\"cloud\"},\"apps\":[]}", &idx, NULL), -1);
    CHECK_EQ(parse("{\"format\":1,\"apps\":[]}", &idx, NULL), 0);           /* empty is fine */
    CHECK_EQ(idx.count, 0);
    /* a string with a raw control character */
    CHECK_EQ(parse("{\"format\":1,\"repo\":{\"name\":\"a\nb\"},\"apps\":[]}", &idx, NULL), -1);
    /* nesting deeper than the reader allows */
    CHECK_EQ(parse("{\"format\":1,\"x\":[[[[[[[[[[[[1]]]]]]]]]]]],\"apps\":[]}", &idx, NULL), -1);
    /* trailing junk after a value inside an app */
    CHECK_EQ(parse("{\"format\":1,\"apps\":[{\"name\":\"a\" \"version\":\"1\"}]}", &idx, NULL), -1);
    /* too large */
    static char big[PURR_REPO_MAX_INDEX + 16];
    memset(big, ' ', sizeof(big));
    memcpy(big, "{\"format\":1,\"apps\":[]}", 22);
    char err[64];
    CHECK_EQ(purr_repo_index_parse(big, sizeof(big), &idx, NULL, err, sizeof(err)), -1);
}

static void test_bad_entries_are_dropped(void)
{
    char hex[65], doc[4096];
    hex_of(BODY, sizeof(BODY) - 1, hex);
    unsigned sz = (unsigned)(sizeof(BODY) - 1);
    snprintf(doc, sizeof(doc),
        "{\"format\":1,\"apps\":["
        "{\"name\":\"good\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a/good.cat\"},"
        "{\"name\":\"bad name\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a/b.cat\"},"
        "{\"name\":\"../x\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a/b.cat\"},"
        "{\"name\":\"nosha\",\"version\":\"1.0.0\",\"size\":%u,\"url\":\"a/b.cat\"},"
        "{\"name\":\"shortsha\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"abcd\",\"url\":\"a/b.cat\"},"
        "{\"name\":\"nonhex\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%.60szzzz\",\"url\":\"a/b.cat\"},"
        "{\"name\":\"zero\",\"version\":\"1.0.0\",\"size\":0,\"sha256\":\"%s\",\"url\":\"a/b.cat\"},"
        "{\"name\":\"huge\",\"version\":\"1.0.0\",\"size\":9999999,\"sha256\":\"%s\",\"url\":\"a/b.cat\"},"
        "{\"name\":\"climb\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"../../etc/x\"},"
        "{\"name\":\"abs\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"/etc/x\"},"
        "{\"name\":\"scheme\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"http://evil/x\"},"
        "{\"name\":\"query\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a/b.cat?x=1\"},"
        "{\"name\":\"badver\",\"version\":\"v-one\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a/b.cat\"},"
        "{\"name\":\"good\",\"version\":\"2.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a/good2.cat\"},"
        "{\"name\":\"noname\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a/b.cat\"},"
        "{}"
        "]}", sz, hex, sz, hex, sz, hex, sz, sz, sz, hex, hex, hex, sz, hex, sz, hex, sz, hex, sz, hex,
        sz, hex, sz, hex, sz, hex);
    purr_repo_index_t idx;
    CHECK_EQ(parse(doc, &idx, NULL), 0);
    CHECK_EQ(idx.count, 2);                          /* "good" and the "noname"... see below */
    CHECK(purr_repo_index_find(&idx, "good") != NULL);
    if (purr_repo_index_find(&idx, "good")) {
        CHECK(strcmp(purr_repo_index_find(&idx, "good")->version, "1.0.0") == 0);   /* the first one stands */
    }
    CHECK(purr_repo_index_find(&idx, "bad name") == NULL);
    CHECK(purr_repo_index_find(&idx, "climb") == NULL);
    CHECK(purr_repo_index_find(&idx, "huge") == NULL);
    CHECK_EQ(idx.count + idx.dropped, 16);           /* every entry is either kept or counted */
}

static void test_kind_policy(void)
{
    char hex[65], doc[1024];
    hex_of(BODY, sizeof(BODY) - 1, hex);
    unsigned sz = (unsigned)(sizeof(BODY) - 1);
    snprintf(doc, sizeof(doc),
        "{\"format\":1,\"repo\":{\"kind\":\"store\"},\"apps\":["
        "{\"name\":\"signedapp\",\"version\":\"1.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a.cat\",\"signed\":true},"
        "{\"name\":\"unsignedapp\",\"version\":\"1.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"b.cat\",\"signed\":false}"
        "]}", sz, hex, sz, hex);
    purr_repo_index_t idx;
    purr_repo_kind_t store = PURR_REPO_STORE, sdk = PURR_REPO_SDK;
    CHECK_EQ(parse(doc, &idx, &store), 0);
    CHECK_EQ(idx.kind, PURR_REPO_STORE);
    CHECK_EQ(idx.count, 1);                          /* a store never offers an unsigned app */
    CHECK_EQ(idx.dropped, 1);
    CHECK(purr_repo_index_find(&idx, "unsignedapp") == NULL);
    CHECK_EQ(parse(doc, &idx, &sdk), -1);            /* fetched as an SDK server, it says it is a store */
    CHECK_EQ(parse(doc, &idx, NULL), 0);             /* no expectation: kept as declared */
    CHECK_EQ(idx.count, 2);

    /* an index that declares nothing cannot be a store, but can be an SDK server */
    const char *nokind = "{\"format\":1,\"apps\":[]}";
    CHECK_EQ(parse(nokind, &idx, &store), -1);
    CHECK_EQ(parse(nokind, &idx, &sdk), 0);
    /* an SDK index cannot be taken for a store */
    const char *sdkdoc = "{\"format\":1,\"repo\":{\"kind\":\"sdk\"},\"apps\":[]}";
    CHECK_EQ(parse(sdkdoc, &idx, &store), -1);
}

static void test_ip_policy(void)
{
    CHECK(purr_repo_ip_is_local(0x0A000202));        /* 10.0.2.2, the VM's host */
    CHECK(purr_repo_ip_is_local(0xC0A80105));        /* 192.168.1.5 */
    CHECK(purr_repo_ip_is_local(0xAC100001));        /* 172.16.0.1 */
    CHECK(purr_repo_ip_is_local(0xAC1FFFFF));        /* 172.31.255.255 */
    CHECK(!purr_repo_ip_is_local(0xAC200001));       /* 172.32.0.1 is public */
    CHECK(!purr_repo_ip_is_local(0xAC0F0001));       /* 172.15.0.1 is public */
    CHECK(purr_repo_ip_is_local(0x7F000001));        /* loopback */
    CHECK(purr_repo_ip_is_local(0xA9FE0101));        /* link-local */
    CHECK(!purr_repo_ip_is_local(0x08080808));       /* 8.8.8.8 */
    CHECK(!purr_repo_ip_is_local(0xC0A90001));       /* 192.169.0.1 */
    CHECK(!purr_repo_ip_is_local(0x64400001));       /* 100.64.0.1 (carrier-grade NAT) */
}

static int sdk_ok(const char *url) { return purr_repo_url_allowed(PURR_REPO_SDK, url, NULL, 0); }
static int store_ok(const char *url) { return purr_repo_url_allowed(PURR_REPO_STORE, url, NULL, 0); }

static void test_url_policy(void)
{
    CHECK(sdk_ok("http://10.0.2.2:47801"));
    CHECK(sdk_ok("http://192.168.1.20:8000/"));
    CHECK(sdk_ok("https://172.16.4.4"));
    CHECK(sdk_ok("http://127.0.0.1:8000"));
    CHECK(sdk_ok("http://169.254.10.10"));
    CHECK(sdk_ok("http://localhost:8000"));
    CHECK(sdk_ok("http://zanders-mac.local:47801/repo"));
    CHECK(sdk_ok("HTTP://10.0.0.1"));
    CHECK(!sdk_ok("http://8.8.8.8"));
    CHECK(!sdk_ok("http://172.32.0.1"));
    CHECK(!sdk_ok("http://example.com"));
    CHECK(!sdk_ok("https://store.purr.example"));
    CHECK(!sdk_ok("http://evil.com/10.0.2.2"));
    CHECK(!sdk_ok("http://10.0.2.2@evil.com"));      /* userinfo trick */
    CHECK(!sdk_ok("http://evil.com@10.0.2.2"));      /* any userinfo at all */
    CHECK(!sdk_ok("http://127.1"));                  /* short forms some resolvers expand */
    CHECK(!sdk_ok("http://2130706433"));             /* a decimal address */
    CHECK(!sdk_ok("http://010.0.0.1"));              /* leading zero: octal in some resolvers */
    CHECK(!sdk_ok("http://10.0.0"));
    CHECK(!sdk_ok("http://10.0.0.1.5"));
    CHECK(!sdk_ok("http://10.0.0.256"));
    CHECK(!sdk_ok("http://[::1]:8000"));
    CHECK(!sdk_ok("http://0x7f.0.0.1"));
    CHECK(!sdk_ok("ftp://10.0.0.1"));
    CHECK(!sdk_ok("file:///etc/passwd"));
    CHECK(!sdk_ok("10.0.0.1"));
    CHECK(!sdk_ok("http://"));
    CHECK(!sdk_ok("http://10.0.0.1:0"));
    CHECK(!sdk_ok("http://10.0.0.1:99999"));
    CHECK(!sdk_ok("http://10.0.0.1:80a"));
    CHECK(!sdk_ok("http://10.0.0.1 /x"));
    CHECK(!sdk_ok("http://10.0.0.1/x y"));
    CHECK(!sdk_ok("http://10.0.0.1\\x"));
    CHECK(!sdk_ok("http://10.0.0.1?x=1"));
    CHECK(!sdk_ok("http://localhost.evil.com"));
    CHECK(!sdk_ok("http://notlocal"));
    CHECK(!sdk_ok(NULL));

    CHECK(store_ok("https://store.example.com"));
    CHECK(store_ok("https://8.8.8.8:8443/x"));
    CHECK(!store_ok("http://store.example.com"));    /* a store is https only */
    CHECK(!store_ok("ftp://store.example.com"));
    CHECK(!store_ok("https://user@store.example.com"));
    CHECK(!store_ok("https://127.1"));

    char why[80] = "";
    CHECK(!purr_repo_url_allowed(PURR_REPO_SDK, "http://8.8.8.8", why, sizeof(why)));
    CHECK(strstr(why, "local network") != NULL);
}

static void test_url_split_and_join(void)
{
    char sch[8], host[96], path[96];
    uint16_t port;
    CHECK_EQ(purr_repo_url_split("http://10.0.2.2:47801/base/x", sch, sizeof(sch), host, sizeof(host), &port, path, sizeof(path)), 0);
    CHECK(strcmp(sch, "http") == 0);
    CHECK(strcmp(host, "10.0.2.2") == 0);
    CHECK_EQ(port, 47801);
    CHECK(strcmp(path, "/base/x") == 0);
    CHECK_EQ(purr_repo_url_split("https://a.b", sch, sizeof(sch), host, sizeof(host), &port, path, sizeof(path)), 0);
    CHECK_EQ(port, 443);
    CHECK(strcmp(path, "/") == 0);
    CHECK_EQ(purr_repo_url_split("HTTP://Host.Local", sch, sizeof(sch), host, sizeof(host), &port, path, sizeof(path)), 0);
    CHECK_EQ(port, 80);
    CHECK(strcmp(sch, "http") == 0);

    char out[160];
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2:47801", "index.json", out, sizeof(out)), 0);
    CHECK(strcmp(out, "http://10.0.2.2:47801/purr/v1/index.json") == 0);
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2:47801/", "apps/clock/1.0.0/package.cat", out, sizeof(out)), 0);
    CHECK(strcmp(out, "http://10.0.2.2:47801/purr/v1/apps/clock/1.0.0/package.cat") == 0);
    CHECK_EQ(purr_repo_url_join("https://store.example.com/shop/", "index.json", out, sizeof(out)), 0);
    CHECK(strcmp(out, "https://store.example.com:443/shop/purr/v1/index.json") == 0);
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2", "../etc/passwd", out, sizeof(out)), -1);
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2", "/etc/passwd", out, sizeof(out)), -1);
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2", "http://evil/x", out, sizeof(out)), -1);
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2", "a//b", out, sizeof(out)), -1);
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2", "", out, sizeof(out)), -1);
    CHECK_EQ(purr_repo_url_join("nonsense", "index.json", out, sizeof(out)), -1);
    char tiny[16];
    CHECK_EQ(purr_repo_url_join("http://10.0.2.2", "index.json", tiny, sizeof(tiny)), -1);   /* never truncates */
}

static void test_versions(void)
{
    CHECK(purr_repo_version_cmp("1.10.0", "1.9.0") > 0);
    CHECK(purr_repo_version_cmp("1.9.0", "1.10.0") < 0);
    CHECK_EQ(purr_repo_version_cmp("1.0", "1.0.0"), 0);
    CHECK_EQ(purr_repo_version_cmp("v2.1", "2.1.0"), 0);
    CHECK(purr_repo_version_cmp("2.0.0", "1.99.99") > 0);
    CHECK(purr_repo_version_cmp("1.0.0-beta", "1.0.0") < 0);       /* a pre-release is older */
    CHECK(purr_repo_version_cmp("1.0.0", "1.0.0-beta") > 0);
    CHECK_EQ(purr_repo_version_cmp("1.0.0-beta", "1.0.0-rc"), 0);  /* suffixes are not ordered among themselves */
    CHECK(purr_repo_version_cmp("1.0.1", "1.0.0-beta") > 0);
    CHECK(purr_repo_version_cmp("0.0.1", "0.0.0") > 0);
}

static void test_updates_and_package_check(void)
{
    char hex[65], doc[2048];
    hex_of(BODY, sizeof(BODY) - 1, hex);
    unsigned sz = (unsigned)(sizeof(BODY) - 1);
    snprintf(doc, sizeof(doc),
        "{\"format\":1,\"apps\":["
        "{\"name\":\"a\",\"version\":\"2.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"a.cat\"},"
        "{\"name\":\"b\",\"version\":\"1.0.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"b.cat\"},"
        "{\"name\":\"c\",\"version\":\"3.1.0\",\"size\":%u,\"sha256\":\"%s\",\"url\":\"c.cat\"}]}",
        sz, hex, sz, hex, sz, hex);
    purr_repo_index_t idx;
    CHECK_EQ(parse(doc, &idx, NULL), 0);
    purr_repo_installed_t have[4];
    memset(have, 0, sizeof(have));
    snprintf(have[0].name, sizeof(have[0].name), "a"); snprintf(have[0].version, sizeof(have[0].version), "1.9.0");   /* older: update */
    snprintf(have[1].name, sizeof(have[1].name), "b"); snprintf(have[1].version, sizeof(have[1].version), "1.0.0");   /* same */
    snprintf(have[2].name, sizeof(have[2].name), "c"); snprintf(have[2].version, sizeof(have[2].version), "4.0.0");   /* newer than the repo */
    snprintf(have[3].name, sizeof(have[3].name), "z"); snprintf(have[3].version, sizeof(have[3].version), "1.0.0");   /* not in the repo */
    int out[4];
    CHECK_EQ(purr_repo_updates(&idx, have, 4, out, 4), 1);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(purr_repo_updates(&idx, have, 4, out, 0), 0);          /* a zero-length answer buffer is safe */

    const purr_repo_app_t *a = &idx.apps[0];
    CHECK_EQ(purr_repo_check_package(a, BODY, sizeof(BODY) - 1), PURR_REPO_PKG_OK);
    CHECK_EQ(purr_repo_check_package(a, BODY, sizeof(BODY) - 2), PURR_REPO_PKG_BAD_SIZE);
    uint8_t tampered[sizeof(BODY)];
    memcpy(tampered, BODY, sizeof(BODY));
    tampered[3] ^= 1;
    CHECK_EQ(purr_repo_check_package(a, tampered, sizeof(BODY) - 1), PURR_REPO_PKG_BAD_HASH);
    CHECK(strcmp(purr_repo_pkg_result_name(PURR_REPO_PKG_BAD_HASH), "wrong hash") == 0);
}

static void test_hex(void)
{
    uint8_t out[32];
    CHECK_EQ(purr_repo_hex_to_sha256("00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF", out), 0);
    CHECK_EQ(out[0], 0x00);
    CHECK_EQ(out[1], 0x11);
    CHECK_EQ(out[15], 0xff);
    CHECK_EQ(out[31], 0xff);
    CHECK_EQ(purr_repo_hex_to_sha256("abc", out), -1);
    CHECK_EQ(purr_repo_hex_to_sha256("", out), -1);
    CHECK_EQ(purr_repo_hex_to_sha256("zz112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", out), -1);
}

int main(void)
{
    test_parse_good();
    test_unknown_fields_and_escapes();
    test_malformed_documents();
    test_bad_entries_are_dropped();
    test_kind_policy();
    test_ip_policy();
    test_url_policy();
    test_url_split_and_join();
    test_versions();
    test_updates_and_package_check();
    test_hex();
    TK_DONE("test_repo");
}

#include "DiscId.hpp"
#include <cstring>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// SHA-1  (FIPS 180-4, compact self-contained implementation)
// ---------------------------------------------------------------------------
namespace {

static inline uint32_t rot32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

struct Sha1Ctx {
    uint32_t state[5];
    uint32_t count[2]; // bit count: [1]=high, [0]=low
    uint8_t  buf[64];
};

static void sha1Block(uint32_t s[5], const uint8_t blk[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)blk[i*4  ] << 24) | ((uint32_t)blk[i*4+1] << 16) |
               ((uint32_t)blk[i*4+2] <<  8) | ((uint32_t)blk[i*4+3]);
    for (int i = 16; i < 80; ++i)
        w[i] = rot32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a=s[0], b=s[1], c=s[2], d=s[3], e=s[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if      (i < 20) { f = (b&c)|(~b&d);           k = 0x5A827999u; }
        else if (i < 40) { f = b^c^d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b&c)|(b&d)|(c&d);       k = 0x8F1BBCDCu; }
        else             { f = b^c^d;                   k = 0xCA62C1D6u; }
        uint32_t t = rot32(a,5) + f + e + k + w[i];
        e=d; d=c; c=rot32(b,30); b=a; a=t;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e;
}

static void sha1Init(Sha1Ctx& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xEFCDAB89u;
    ctx.state[2] = 0x98BADCFEu;
    ctx.state[3] = 0x10325476u;
    ctx.state[4] = 0xC3D2E1F0u;
    ctx.count[0] = ctx.count[1] = 0;
    memset(ctx.buf, 0, 64);
}

static void sha1Update(Sha1Ctx& ctx, const uint8_t* data, size_t len) {
    // Byte offset of where we are inside the buffer
    uint32_t j = (ctx.count[0] >> 3) & 63u;

    // Update bit count
    if ((ctx.count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3))
        ++ctx.count[1];
    ctx.count[1] += (uint32_t)(len >> 29);

    size_t i = 0;
    if (j + len >= 64) {
        // Fill up the buffer and process it
        size_t fill = 64 - j;
        memcpy(&ctx.buf[j], data, fill);
        sha1Block(ctx.state, ctx.buf);
        i = fill;
        // Process any remaining full blocks
        for (; i + 63 < len; i += 64)
            sha1Block(ctx.state, data + i);
        j = 0;
    }
    // Stash the remainder
    memcpy(&ctx.buf[j], data + i, len - i);
}

static void sha1Final(Sha1Ctx& ctx, uint8_t digest[20]) {
    // Store the bit count (big-endian 64-bit)
    uint8_t bits[8];
    for (int i = 0; i < 8; ++i)
        bits[i] = (uint8_t)((ctx.count[(i < 4) ? 1 : 0] >> ((3 - (i & 3)) * 8)) & 0xFF);

    // Append 0x80 padding
    uint8_t pad = 0x80;
    sha1Update(ctx, &pad, 1);

    // Pad with zeros until 56 bytes into the block
    pad = 0x00;
    while ((ctx.count[0] & 504u) != 448u)
        sha1Update(ctx, &pad, 1);

    // Append original bit length
    sha1Update(ctx, bits, 8);

    // Unpack state to bytes
    for (int i = 0; i < 20; ++i)
        digest[i] = (uint8_t)((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
}

// ---------------------------------------------------------------------------
// MusicBrainz modified Base64:  + → .   / → _   = → -
// ---------------------------------------------------------------------------
static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string mbBase64(const uint8_t* src, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t g  = (uint32_t)src[i] << 16;
        if (i+1 < len) g |= (uint32_t)src[i+1] << 8;
        if (i+2 < len) g |= (uint32_t)src[i+2];
        out += kB64[(g >> 18) & 0x3F];
        out += kB64[(g >> 12) & 0x3F];
        out += (i+1 < len) ? kB64[(g >>  6) & 0x3F] : '=';
        out += (i+2 < len) ? kB64[(g      ) & 0x3F] : '=';
    }
    for (char& c : out) {
        if      (c == '+') c = '.';
        else if (c == '/') c = '_';
        else if (c == '=') c = '-';
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

namespace atomicripper::metadata {

std::string DiscId::calculate(const drive::TOC& toc) {
    if (!toc.isValid())
        return {};

    // Build the 804-character ASCII input string:
    //   [0..1]   first track (2 uppercase hex chars)
    //   [2..3]   last  track (2 uppercase hex chars)
    //   [4..11]  lead-out offset in sectors + 150 (8 uppercase hex chars)
    //   [12..819] tracks 1–99 offsets, each 8 uppercase hex chars, zero-padded
    char buf[805];  // 804 chars + null terminator for snprintf safety
    memset(buf, '0', 804);
    buf[804] = '\0';

    snprintf(buf + 0, 3, "%02X", toc.firstTrack);
    snprintf(buf + 2, 3, "%02X", toc.lastTrack);
    snprintf(buf + 4, 9, "%08X", toc.leadOutLBA + 150);

    for (const auto& t : toc.tracks) {
        if (t.number < 1 || t.number > 99) continue;
        // Slot for track N starts at offset 4 + N*8
        snprintf(buf + 4 + t.number * 8, 9, "%08X", t.lba + 150);
    }
    // Restore the null byte that snprintf may have advanced past position 803
    buf[804] = '\0';

    Sha1Ctx ctx;
    sha1Init(ctx);
    sha1Update(ctx, reinterpret_cast<const uint8_t*>(buf), 804);
    uint8_t hash[20];
    sha1Final(ctx, hash);

    return mbBase64(hash, 20);
}

std::string DiscId::lookupUrl(const std::string& discId) {
    return "https://musicbrainz.org/cdtoc/attach?id=" + discId;
}

} // namespace atomicripper::metadata

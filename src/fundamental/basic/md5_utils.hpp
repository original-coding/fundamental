#pragma once

#include <cstdlib>
#include <cstring>

#include <string>

namespace Fundamental {

/*
 *  MD5 md5;
 *  md5.Update(streamChunk1,streamChunk1Length);
 *  md5.Update(streamChunk2,streamChunk2Length);
 *  ...;
 *  md5.Update(streamChunkLast,streamChunkLastLength);
 *
 *  //mark the end
 *  md5.Finalize();
 *  //Now,calling function 'Update' will make no sense
 *
 * //get the md5 string
 *  std::string md5sum=md5.HexDigest();
 */
class MD5 {
public:
    MD5() {
        Init();
    }
    /*
     * update the md5 generate context by the input stream data
     */
    void Update(const uint8_t* input, std::uint32_t input_length) {
        if (input == nullptr || input_length == 0) return;

        std::uint32_t input_index, buffer_index;
        std::uint32_t buffer_space; // how much space is left in buffer

        if (finalized) return;
        // Compute number of bytes mod 64
        buffer_index = ((count[0] >> 3) & 0x3F);

        // Update number of bits
        if ((count[0] += (input_length << 3)) < (input_length << 3)) count[1]++;

        count[1] += input_length >> 29;

        buffer_space = 64 - buffer_index; // how much space is left in buffer

        // Transform as many times as possible.
        if (input_length >= buffer_space) {
            // ie. we have enough to fill the buffer
            // fill the rest of the buffer and transform
            std::memcpy(buffer + buffer_index, input, buffer_space);
            transform(buffer);

            // now, transform each 64-byte piece of the input, bypassing the buffer
            for (input_index = buffer_space; input_index + 63 < input_length; input_index += 64)
                transform(input + input_index);

            buffer_index = 0; // so we can buffer remaining
        } else
            input_index = 0; // so we can buffer the whole input

        // and here we do the buffering:
        std::memcpy(buffer + buffer_index, input + input_index, input_length - input_index);
    }
    /*
     * mark the input stream's end and update the final md5 digest buffer
     */
    void Finalize() {
        static std::uint8_t PADDING[64] = { 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                            0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                            0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

        if (finalized) return;

        // Save number of bits
        std::uint8_t bits[8] = { 0 };
        encode(bits, count, 8);

        // Pad out to 56 mod 64.
        std::uint32_t index  = ((count[0] >> 3) & 0x3f);
        std::uint32_t padLen = (index < 56) ? (56 - index) : (120 - index);
        Update(PADDING, padLen);

        // Append length (before padding)
        Update(bits, 8);

        // Store state in digest
        encode(digest, state, 16);

        // Zeroize sensitive information
        memset(buffer, 0, sizeof(*buffer));

        finalized = 1;
    }
    /*
     * dump md5 digest buffer to a 32 bytes string in lowercase
     */
    std::string HexDigest() //	digest as a 33-byte ascii-hex string
    {
        std::string hex;
        if (finalized) {
            hex.resize(33);
            for (int i = 0; i < 16; i++)
                snprintf(hex.data() + i * 2, 3, "%02x", digest[i]);
        }
        return hex.empty() ? hex : hex.substr(0, hex.size() - 1);
    }

private:
    MD5(const MD5&)            = delete;
    MD5(MD5&&)                 = delete;
    MD5& operator=(const MD5&) = delete;
    MD5& operator=(MD5&&)      = delete;

    void Init() // called by all constructors
    {
        finalized = 0; // we just started!

        // Nothing counted, so count=0
        count[0] = 0;
        count[1] = 0;

        // Load magic initialization constants.
        state[0] = 0x67452301;
        state[1] = 0xefcdab89;
        state[2] = 0x98badcfe;
        state[3] = 0x10325476;
    }

    void transform(const std::uint8_t* block) // does the real update work.  Note that length is implied to be 64.
    {
        std::uint32_t constexpr S11 = 7;
        std::uint32_t constexpr S12 = 12;
        std::uint32_t constexpr S13 = 17;
        std::uint32_t constexpr S14 = 22;
        std::uint32_t constexpr S21 = 5;
        std::uint32_t constexpr S22 = 9;
        std::uint32_t constexpr S23 = 14;
        std::uint32_t constexpr S24 = 20;
        std::uint32_t constexpr S31 = 4;
        std::uint32_t constexpr S32 = 11;
        std::uint32_t constexpr S33 = 16;
        std::uint32_t constexpr S34 = 23;
        std::uint32_t constexpr S41 = 6;
        std::uint32_t constexpr S42 = 10;
        std::uint32_t constexpr S43 = 15;
        std::uint32_t constexpr S44 = 21;

        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];

        decode(x, block, 64);

        // assert(!finalized); // not just a user error, since the method is private

        /* Round 1 */
        FF(a, b, c, d, x[0], S11, 0xd76aa478);  /* 1 */
        FF(d, a, b, c, x[1], S12, 0xe8c7b756);  /* 2 */
        FF(c, d, a, b, x[2], S13, 0x242070db);  /* 3 */
        FF(b, c, d, a, x[3], S14, 0xc1bdceee);  /* 4 */
        FF(a, b, c, d, x[4], S11, 0xf57c0faf);  /* 5 */
        FF(d, a, b, c, x[5], S12, 0x4787c62a);  /* 6 */
        FF(c, d, a, b, x[6], S13, 0xa8304613);  /* 7 */
        FF(b, c, d, a, x[7], S14, 0xfd469501);  /* 8 */
        FF(a, b, c, d, x[8], S11, 0x698098d8);  /* 9 */
        FF(d, a, b, c, x[9], S12, 0x8b44f7af);  /* 10 */
        FF(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
        FF(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
        FF(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
        FF(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
        FF(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
        FF(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

        /* Round 2 */
        GG(a, b, c, d, x[1], S21, 0xf61e2562);  /* 17 */
        GG(d, a, b, c, x[6], S22, 0xc040b340);  /* 18 */
        GG(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
        GG(b, c, d, a, x[0], S24, 0xe9b6c7aa);  /* 20 */
        GG(a, b, c, d, x[5], S21, 0xd62f105d);  /* 21 */
        GG(d, a, b, c, x[10], S22, 0x2441453);  /* 22 */
        GG(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
        GG(b, c, d, a, x[4], S24, 0xe7d3fbc8);  /* 24 */
        GG(a, b, c, d, x[9], S21, 0x21e1cde6);  /* 25 */
        GG(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
        GG(c, d, a, b, x[3], S23, 0xf4d50d87);  /* 27 */
        GG(b, c, d, a, x[8], S24, 0x455a14ed);  /* 28 */
        GG(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
        GG(d, a, b, c, x[2], S22, 0xfcefa3f8);  /* 30 */
        GG(c, d, a, b, x[7], S23, 0x676f02d9);  /* 31 */
        GG(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

        /* Round 3 */
        HH(a, b, c, d, x[5], S31, 0xfffa3942);  /* 33 */
        HH(d, a, b, c, x[8], S32, 0x8771f681);  /* 34 */
        HH(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
        HH(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
        HH(a, b, c, d, x[1], S31, 0xa4beea44);  /* 37 */
        HH(d, a, b, c, x[4], S32, 0x4bdecfa9);  /* 38 */
        HH(c, d, a, b, x[7], S33, 0xf6bb4b60);  /* 39 */
        HH(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
        HH(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
        HH(d, a, b, c, x[0], S32, 0xeaa127fa);  /* 42 */
        HH(c, d, a, b, x[3], S33, 0xd4ef3085);  /* 43 */
        HH(b, c, d, a, x[6], S34, 0x4881d05);   /* 44 */
        HH(a, b, c, d, x[9], S31, 0xd9d4d039);  /* 45 */
        HH(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
        HH(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
        HH(b, c, d, a, x[2], S34, 0xc4ac5665);  /* 48 */

        /* Round 4 */
        II(a, b, c, d, x[0], S41, 0xf4292244);  /* 49 */
        II(d, a, b, c, x[7], S42, 0x432aff97);  /* 50 */
        II(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
        II(b, c, d, a, x[5], S44, 0xfc93a039);  /* 52 */
        II(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
        II(d, a, b, c, x[3], S42, 0x8f0ccc92);  /* 54 */
        II(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
        II(b, c, d, a, x[1], S44, 0x85845dd1);  /* 56 */
        II(a, b, c, d, x[8], S41, 0x6fa87e4f);  /* 57 */
        II(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
        II(c, d, a, b, x[6], S43, 0xa3014314);  /* 59 */
        II(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
        II(a, b, c, d, x[4], S41, 0xf7537e82);  /* 61 */
        II(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
        II(c, d, a, b, x[2], S43, 0x2ad7d2bb);  /* 63 */
        II(b, c, d, a, x[9], S44, 0xeb86d391);  /* 64 */

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;

        // Zeroize sensitive information.
        std::memset((std::uint8_t*)x, 0, sizeof(x));
    }

    void encode(std::uint8_t* dest, std::uint32_t* src, std::uint32_t length) {
        for (std::uint32_t i = 0, j = 0; j < length; i++, j += 4) {
            dest[j]     = (std::uint8_t)(src[i] & 0xff);
            dest[j + 1] = (std::uint8_t)((src[i] >> 8) & 0xff);
            dest[j + 2] = (std::uint8_t)((src[i] >> 16) & 0xff);
            dest[j + 3] = (std::uint8_t)((src[i] >> 24) & 0xff);
        }
    }

    void decode(std::uint32_t* dest, const std::uint8_t* src, std::uint32_t length) {
        for (std::uint32_t i = 0, j = 0; j < length; i++, j += 4)
            dest[i] = ((std::uint32_t)src[j]) | (((std::uint32_t)src[j + 1]) << 8) |
                      (((std::uint32_t)src[j + 2]) << 16) | (((std::uint32_t)src[j + 3]) << 24);
    }

    std::uint32_t rotate_left(std::uint32_t x, std::uint32_t n) {
        return (x << n) | (x >> (32 - n));
    }

    std::uint32_t F(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (x & y) | (~x & z);
    }

    std::uint32_t G(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (x & z) | (y & ~z);
    }

    std::uint32_t H(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return x ^ y ^ z;
    }

    std::uint32_t I(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return y ^ (x | ~z);
    }

    void FF(std::uint32_t& a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t x, std::uint32_t s,
            std::uint32_t ac) {
        a += F(b, c, d) + x + ac;
        a = rotate_left(a, s) + b;
    }

    void GG(std::uint32_t& a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t x, std::uint32_t s,
            std::uint32_t ac) {
        a += G(b, c, d) + x + ac;
        a = rotate_left(a, s) + b;
    }

    void HH(std::uint32_t& a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t x, std::uint32_t s,
            std::uint32_t ac) {
        a += H(b, c, d) + x + ac;
        a = rotate_left(a, s) + b;
    }

    void II(std::uint32_t& a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t x, std::uint32_t s,
            std::uint32_t ac) {
        a += I(b, c, d) + x + ac;
        a = rotate_left(a, s) + b;
    }

private:
    std::uint32_t state[4]  = { 0 };
    std::uint32_t count[2]  = { 0 }; // number of *bits*, mod 2^64
    std::uint8_t buffer[64] = { 0 }; // input buffer
    std::uint8_t digest[16] = { 0 };
    std::uint8_t finalized  = 0;
};

std::string inline CryptMD5(const void* inbuf, std::uint32_t buflen) {
    if (NULL == inbuf || buflen == 0) return std::string();

    MD5 md5;
    md5.Update(reinterpret_cast<const std::uint8_t*>(inbuf), buflen);
    md5.Finalize();
    return md5.HexDigest();
}

} // namespace Fundamental

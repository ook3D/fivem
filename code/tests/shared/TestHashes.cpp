#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstring>
#include <string>

#include <fnv.h>
#include <SHA1.h>

namespace
{
std::string ToHex(const uint8_t* data, size_t length)
{
	static const char digits[] = "0123456789abcdef";

	std::string result;
	for (size_t i = 0; i < length; i++)
	{
		result += digits[data[i] >> 4];
		result += digits[data[i] & 0xF];
	}

	return result;
}

std::string Sha1Hex(const std::string& input)
{
	sha1nfo info;
	sha1_init(&info);
	sha1_write(&info, input.data(), input.size());

	return ToHex(sha1_result(&info), HASH_LENGTH);
}
}

TEST_CASE("FNV-1a 32-bit matches the published test vectors")
{
	fnv1a_t<4> hash;

	REQUIRE(hash("") == 0x811C9DC5u);
	REQUIRE(hash("a") == 0xE40C292Cu);
	REQUIRE(hash("foobar") == 0xBF9CF968u);
}

TEST_CASE("FNV-1a 64-bit matches the published test vectors")
{
	fnv1a_t<8> hash;

	REQUIRE(hash("") == 0xCBF29CE484222325ull);
	REQUIRE(hash("a") == 0xAF63DC4C8601EC8Cull);
	REQUIRE(hash("foobar") == 0x85944171F73967E8ull);
}

TEST_CASE("FNV-1a overloads agree for the same input")
{
	const std::string text = "foobar";

	fnv1a_t<4> hash32;
	REQUIRE(hash32(text) == 0xBF9CF968u);
	REQUIRE(hash32(text.data(), text.size()) == 0xBF9CF968u);
	REQUIRE(hash32(text.c_str()) == 0xBF9CF968u);
	REQUIRE(fnv1a_t<4>::Hash("foobar") == 0xBF9CF968u);

	fnv1a_t<8> hash64;
	REQUIRE(hash64(text) == 0x85944171F73967E8ull);
	REQUIRE(hash64(text.data(), text.size()) == 0x85944171F73967E8ull);
	REQUIRE(hash64(text.c_str()) == 0x85944171F73967E8ull);
	REQUIRE(fnv1a_t<8>::Hash("foobar") == 0x85944171F73967E8ull);
}

TEST_CASE("FNV-1a length overload hashes exactly the given bytes")
{
	fnv1a_t<4> hash;

	REQUIRE(hash("foobar", 1) == hash("f"));
	REQUIRE(hash("foobar", 0) == 0x811C9DC5u);
	REQUIRE(hash("foobar", 6) == hash("foobar"));
}

TEST_CASE("FNV-1a lowercase variant maps uppercase letters to lowercase")
{
	REQUIRE(fnv1a_lower_t<4>{}("ABC") == fnv1a_t<4>{}("abc"));
	REQUIRE(fnv1a_lower_t<8>{}("ABC") == fnv1a_t<8>{}("abc"));
}

TEST_CASE("Compile-time StringHash includes the trailing NUL of the literal")
{
	fnv1a_t<4> hash;

	REQUIRE(static_cast<unsigned int>(StringHash("a")) == hash("a") * 16777619u);
	REQUIRE(static_cast<unsigned int>(StringHash("foobar")) == hash("foobar") * 16777619u);
	REQUIRE(static_cast<unsigned int>(StringHash("")) == hash("") * 16777619u);
	REQUIRE(static_cast<unsigned int>(StringHash("a")) != hash("a"));
}

TEST_CASE("SHA-1 matches the published test vectors")
{
	REQUIRE(Sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	REQUIRE(Sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
	REQUIRE(Sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("SHA-1 of one million repeated 'a' written in chunks matches the published vector")
{
	const std::string chunk(1000, 'a');

	sha1nfo info;
	sha1_init(&info);
	for (int i = 0; i < 1000; i++)
	{
		sha1_write(&info, chunk.data(), chunk.size());
	}

	REQUIRE(ToHex(sha1_result(&info), HASH_LENGTH) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("SHA-1 byte-by-byte writes equal a single bulk write")
{
	const std::string input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

	sha1nfo info;
	sha1_init(&info);
	for (char c : input)
	{
		sha1_writebyte(&info, static_cast<uint8_t>(c));
	}

	REQUIRE(ToHex(sha1_result(&info), HASH_LENGTH) == Sha1Hex(input));
}

TEST_CASE("HMAC-SHA1 matches the RFC 2202 test vectors")
{
	sha1nfo info;

	const uint8_t key1[20] = { 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b };
	sha1_initHmac(&info, key1, sizeof(key1));
	sha1_write(&info, "Hi There", 8);
	REQUIRE(ToHex(sha1_resultHmac(&info), HASH_LENGTH) == "b617318655057264e28bc0b6fb378c8ef146be00");

	const char data2[] = "what do ya want for nothing?";
	sha1_initHmac(&info, reinterpret_cast<const uint8_t*>("Jefe"), 4);
	sha1_write(&info, data2, strlen(data2));
	REQUIRE(ToHex(sha1_resultHmac(&info), HASH_LENGTH) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
}

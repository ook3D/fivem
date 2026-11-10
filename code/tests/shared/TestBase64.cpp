#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <base64.h>

namespace
{
std::string EncodeToString(const std::vector<unsigned char>& input, size_t* outLength = nullptr)
{
	size_t length = 0;
	char* encoded = base64_encode(input.data(), input.size(), &length);

	REQUIRE(encoded != nullptr);

	std::string result(encoded, length);
	free(encoded);

	if (outLength)
	{
		*outLength = length;
	}

	return result;
}

std::vector<unsigned char> BytesOf(const std::string& text)
{
	return { text.begin(), text.end() };
}

bool DecodeToBytes(const std::string& encoded, std::vector<unsigned char>& out)
{
	size_t length = 0;
	unsigned char* decoded = base64_decode(encoded.data(), encoded.size(), &length);

	if (!decoded)
	{
		return false;
	}

	out.assign(decoded, decoded + length);
	free(decoded);

	return true;
}
}

TEST_CASE("Base64 encodes the RFC 4648 test vectors")
{
	const std::pair<std::string, std::string> vectors[] = {
		{ "f", "Zg==" },
		{ "fo", "Zm8=" },
		{ "foo", "Zm9v" },
		{ "foob", "Zm9vYg==" },
		{ "fooba", "Zm9vYmE=" },
		{ "foobar", "Zm9vYmFy" },
	};

	for (const auto& vector : vectors)
	{
		size_t length = 0;
		REQUIRE(EncodeToString(BytesOf(vector.first), &length) == vector.second);
		REQUIRE(length == 4 * ((vector.first.size() + 2) / 3));
	}
}

TEST_CASE("Base64 decodes the RFC 4648 test vectors")
{
	const std::pair<std::string, std::string> vectors[] = {
		{ "Zg==", "f" },
		{ "Zm8=", "fo" },
		{ "Zm9v", "foo" },
		{ "Zm9vYg==", "foob" },
		{ "Zm9vYmE=", "fooba" },
		{ "Zm9vYmFy", "foobar" },
	};

	for (const auto& vector : vectors)
	{
		std::vector<unsigned char> decoded;
		REQUIRE(DecodeToBytes(vector.first, decoded));
		REQUIRE(decoded == BytesOf(vector.second));
	}
}

TEST_CASE("Base64 round trips every byte value")
{
	std::vector<unsigned char> input(256);
	for (size_t i = 0; i < input.size(); i++)
	{
		input[i] = static_cast<unsigned char>(i);
	}

	std::vector<unsigned char> decoded;
	REQUIRE(DecodeToBytes(EncodeToString(input), decoded));
	REQUIRE(decoded == input);
}

TEST_CASE("Base64 round trips pseudo-random buffers of every length up to 64")
{
	uint32_t state = 0x12345678;

	for (size_t length = 1; length <= 64; length++)
	{
		std::vector<unsigned char> input(length);
		for (auto& byte : input)
		{
			state = state * 1664525u + 1013904223u;
			byte = static_cast<unsigned char>(state >> 24);
		}

		size_t encodedLength = 0;
		std::string encoded = EncodeToString(input, &encodedLength);
		REQUIRE(encodedLength == 4 * ((length + 2) / 3));
		REQUIRE(encoded.size() % 4 == 0);

		std::vector<unsigned char> decoded;
		REQUIRE(DecodeToBytes(encoded, decoded));
		REQUIRE(decoded == input);
	}
}

TEST_CASE("Base64 decode rejects input whose length is not a multiple of four")
{
	const std::string invalidInputs[] = { "Z", "Zg", "Zg=", "Zm9vY", "Zm9vYm" };

	for (const auto& invalid : invalidInputs)
	{
		size_t length = 0;
		REQUIRE(base64_decode(invalid.data(), invalid.size(), &length) == nullptr);
	}
}

#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <Utils.h>

static std::string Encode(const char* in, size_t len)
{
	return std::string(url_encode(fwString(in, len)).c_str());
}

TEST_CASE("url_encode leaves unreserved characters unchanged")
{
	REQUIRE(Encode("abcXYZ019-_.~", 13) == "abcXYZ019-_.~");
}

TEST_CASE("url_encode handles empty input")
{
	REQUIRE(Encode("", 0).empty());
}

TEST_CASE("url_encode turns spaces into plus signs")
{
	REQUIRE(Encode("a b", 3) == "a+b");
}

TEST_CASE("url_encode escapes a literal plus sign in lowercase hex")
{
	REQUIRE(Encode("a+b", 3) == "a%2bb");
}

TEST_CASE("url_encode escapes bytes as zero padded lowercase hex")
{
	REQUIRE(Encode("\xC3\xA9", 2) == "%c3%a9");
	REQUIRE(Encode("\x0a", 1) == "%0a");
}

TEST_CASE("ToWide and ToNarrow handle empty strings")
{
	REQUIRE(ToWide(std::string_view{}).empty());
	REQUIRE(ToNarrow(std::wstring_view{}).empty());
}

TEST_CASE("ToWide and ToNarrow round trip ASCII")
{
	REQUIRE(ToWide("hello") == L"hello");
	REQUIRE(ToNarrow(L"hello") == "hello");
}

TEST_CASE("ToWide and ToNarrow round trip multibyte text")
{
	std::string utf8 = "h\xC3\xA9llo \xE2\x82\xAC";

	std::wstring wide = ToWide(utf8);
	REQUIRE(wide.size() == 7);
	REQUIRE(wide[1] == 0x00E9);
	REQUIRE(wide[6] == 0x20AC);
	REQUIRE(ToNarrow(wide) == utf8);
}

TEST_CASE("ToWide and ToNarrow round trip a non-BMP code point")
{
	std::string utf8 = "\xF0\x9F\x98\x80";

	std::wstring wide = ToWide(utf8);

	if (sizeof(wchar_t) == 2)
	{
		REQUIRE(wide.size() == 2);
	}
	else
	{
		REQUIRE(wide.size() == 1);
		REQUIRE(wide[0] == 0x1F600);
	}

	REQUIRE(ToNarrow(wide) == utf8);
}

TEST_CASE("ToWide replaces invalid UTF-8 with the replacement character")
{
	std::wstring wide = ToWide(std::string_view("\xff", 1));

	REQUIRE(wide.size() == 1);
	REQUIRE(wide[0] == 0xFFFD);
}

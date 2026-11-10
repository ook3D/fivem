#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <PlayerName.h>

TEST_CASE("NormalizePlayerName leaves a plain ASCII name unchanged")
{
	std::string name = "Alice";

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name == "Alice");
}

TEST_CASE("NormalizePlayerName keeps an empty name empty")
{
	std::string name;

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name.empty());
}

TEST_CASE("NormalizePlayerName keeps a name of exactly the maximum length")
{
	std::string name(fx::kMaxPlayerNameLength, 'a');

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name.size() == fx::kMaxPlayerNameLength);
	REQUIRE(name == std::string(fx::kMaxPlayerNameLength, 'a'));
}

TEST_CASE("NormalizePlayerName keeps a name one byte below the maximum length")
{
	std::string name(fx::kMaxPlayerNameLength - 1, 'a');

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name.size() == fx::kMaxPlayerNameLength - 1);
}

TEST_CASE("NormalizePlayerName truncates an oversized name to the maximum length")
{
	std::string name(300, 'a');

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name.size() == fx::kMaxPlayerNameLength);
	REQUIRE(name == std::string(fx::kMaxPlayerNameLength, 'a'));
}

TEST_CASE("NormalizePlayerName replaces a multibyte sequence split by truncation")
{
	std::string name(fx::kMaxPlayerNameLength - 1, 'a');
	name += "\xC3\xA9";

	REQUIRE(fx::NormalizePlayerName(name));

	// the cut leaves a dangling lead byte, which becomes one 3-byte U+FFFD
	REQUIRE(name.size() == (fx::kMaxPlayerNameLength - 1) + 3);
	REQUIRE(name.substr(0, fx::kMaxPlayerNameLength - 1) == std::string(fx::kMaxPlayerNameLength - 1, 'a'));
	REQUIRE(name.substr(fx::kMaxPlayerNameLength - 1) == "\xEF\xBF\xBD");
	REQUIRE(utf8::is_valid(name.begin(), name.end()));
}

TEST_CASE("NormalizePlayerName keeps a complete multibyte character at the cut")
{
	std::string name(fx::kMaxPlayerNameLength - 2, 'a');
	name += "\xC3\xA9";
	name += "tail";

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name.size() == fx::kMaxPlayerNameLength);
	REQUIRE(name.substr(fx::kMaxPlayerNameLength - 2) == "\xC3\xA9");
}

TEST_CASE("NormalizePlayerName replaces an invalid lead byte")
{
	std::string name = "\xFF";

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name == "\xEF\xBF\xBD");
}

TEST_CASE("NormalizePlayerName replaces an encoded surrogate with a single marker")
{
	std::string name = "\xED\xA0\x80";

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name == "\xEF\xBF\xBD");
}

TEST_CASE("NormalizePlayerName keeps valid multibyte characters")
{
	std::string name = "\xC3\xA9\xE2\x82\xAC";

	REQUIRE(fx::NormalizePlayerName(name));
	REQUIRE(name == "\xC3\xA9\xE2\x82\xAC");
}

TEST_CASE("NormalizePlayerName always produces valid UTF-8")
{
	const std::string inputs[] = {
		std::string("\xFF\xFE\xFD"),
		std::string("abc\x80\x80\x80xyz"),
		std::string("\xC0\xAF"),
		std::string("\xF4\x90\x80\x80"),
		std::string("\xE2\x82"),
		std::string("ok\xED\xA0\x80ok"),
		std::string("a\0b", 3),
		std::string(250, '\xC3'),
	};

	for (std::string name : inputs)
	{
		REQUIRE(fx::NormalizePlayerName(name));
		REQUIRE(utf8::is_valid(name.begin(), name.end()));
	}
}

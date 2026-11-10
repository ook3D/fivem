#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <GameBuilds.h>

namespace
{
bool ParseBuild(const std::string& input, std::string& result)
{
	fx::GameBuild build;
	bool ok = ConsoleArgumentType<fx::GameBuild>::Parse(input, &build);
	result = build;
	return ok;
}
}

TEST_CASE("GameBuild parses a canonical integer as is")
{
	std::string result;

	REQUIRE(ParseBuild("2060", result));
	REQUIRE(result == "2060");

	REQUIRE(ParseBuild("3889", result));
	REQUIRE(result == "3889");
}

TEST_CASE("GameBuild rejects a non-canonical integer")
{
	std::string result;

	REQUIRE_FALSE(ParseBuild("007", result));
	REQUIRE_FALSE(ParseBuild("2060abc", result));
}

TEST_CASE("GameBuild rejects empty input")
{
	std::string result;

	REQUIRE_FALSE(ParseBuild("", result));
}

TEST_CASE("GameBuild resolves aliases with and without the mp prefix")
{
	std::string result;

	REQUIRE(ParseBuild("sum", result));
	REQUIRE(result == "2060");

	REQUIRE(ParseBuild("mpsum", result));
	REQUIRE(result == "2060");

	REQUIRE(ParseBuild("tuner", result));
	REQUIRE(result == "2372");

	REQUIRE(ParseBuild("security", result));
	REQUIRE(result == "2545");

	REQUIRE(ParseBuild("g9ec", result));
	REQUIRE(result == "2612");

	REQUIRE(ParseBuild("sum2", result));
	REQUIRE(result == "2699");

	REQUIRE(ParseBuild("christmas3", result));
	REQUIRE(result == "2802");
}

TEST_CASE("GameBuild expands the heist shorthand")
{
	std::string result;

	REQUIRE(ParseBuild("h4", result));
	REQUIRE(result == "2189");

	REQUIRE(ParseBuild("mpheist4", result));
	REQUIRE(result == "2189");

	REQUIRE_FALSE(ParseBuild("hx", result));
}

TEST_CASE("GameBuild expands the christmas shorthand")
{
	std::string result;

	REQUIRE(ParseBuild("xm18", result));
	REQUIRE(result == "1604");

	REQUIRE_FALSE(ParseBuild("xm3", result));
}

TEST_CASE("GameBuild resolves the dated aliases")
{
	std::string result;

	REQUIRE(ParseBuild("2023_01", result));
	REQUIRE(result == "2944");

	REQUIRE(ParseBuild("2024_02", result));
	REQUIRE(result == "3407");

	REQUIRE(ParseBuild("2025_02", result));
	REQUIRE(result == "3751");

	REQUIRE(ParseBuild("2026_patch_01", result));
	REQUIRE(result == "3788");

	REQUIRE(ParseBuild("mp2026_01", result));
	REQUIRE(result == "3889");
}

TEST_CASE("GameBuild matches aliases case-insensitively only after the mp prefix")
{
	std::string result;

	REQUIRE(ParseBuild("mpSUM", result));
	REQUIRE(result == "2060");

	REQUIRE_FALSE(ParseBuild("MPSUM", result));
}

TEST_CASE("GameBuild rejects unknown aliases")
{
	std::string result;

	REQUIRE_FALSE(ParseBuild("nonsense", result));
	REQUIRE_FALSE(ParseBuild("mp", result));
}

TEST_CASE("GameBuild unparse returns the build string")
{
	REQUIRE(ConsoleArgumentType<fx::GameBuild>::Unparse(fx::GameBuild("2699")) == "2699");
}

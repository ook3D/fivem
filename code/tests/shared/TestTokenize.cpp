#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <console/Console.h>

using Args = std::vector<std::string>;

TEST_CASE("Tokenize returns no arguments for an empty line")
{
	auto result = console::Tokenize("");

	REQUIRE(result.Count() == 0);
}

TEST_CASE("Tokenize returns no arguments for a whitespace-only line")
{
	REQUIRE(console::Tokenize("   \t  ").Count() == 0);
}

TEST_CASE("Tokenize splits arguments on whitespace")
{
	auto result = console::Tokenize("set foo bar");

	REQUIRE(result.GetArguments() == Args{ "set", "foo", "bar" });
}

TEST_CASE("Tokenize collapses leading, trailing, repeated and tab whitespace")
{
	REQUIRE(console::Tokenize("  set \t foo\t\tbar   ").GetArguments() == Args{ "set", "foo", "bar" });
}

TEST_CASE("Tokenize keeps a quoted string as one argument")
{
	auto result = console::Tokenize("echo \"hello world\"");

	REQUIRE(result.GetArguments() == Args{ "echo", "hello world" });
}

TEST_CASE("Tokenize handles an escaped quote inside a quoted string")
{
	auto result = console::Tokenize("echo \"a\\\"b\"");

	REQUIRE(result.GetArguments() == Args{ "echo", "a\"b" });
}

TEST_CASE("Tokenize treats an unterminated quote as running to the end of the line")
{
	auto result = console::Tokenize("\"unterminated");

	REQUIRE(result.GetArguments() == Args{ "unterminated" });
}

TEST_CASE("Tokenize does not treat a double slash inside a token as a comment")
{
	REQUIRE(console::Tokenize("a//b").GetArguments() == Args{ "a//b" });
}

TEST_CASE("Tokenize strips a double slash comment that starts a token")
{
	REQUIRE(console::Tokenize("foo // bar").GetArguments() == Args{ "foo" });
}

TEST_CASE("Tokenize strips a hash comment that starts a token")
{
	REQUIRE(console::Tokenize("say hi # note").GetArguments() == Args{ "say", "hi" });
	REQUIRE(console::Tokenize("# note").Count() == 0);
}

TEST_CASE("Tokenize does not treat a hash inside a token as a comment")
{
	REQUIRE(console::Tokenize("say hi#note").GetArguments() == Args{ "say", "hi#note" });
}

TEST_CASE("Tokenize returns no arguments for invalid UTF-8")
{
	REQUIRE(console::Tokenize("\xff").Count() == 0);
}

TEST_CASE("Tokenize round-trips non-ASCII UTF-8 arguments")
{
	auto result = console::Tokenize("say caf\xc3\xa9 \"na\xc3\xafve text\"");

	REQUIRE(result.GetArguments() == Args{ "say", "caf\xc3\xa9", "na\xc3\xafve text" });
}

#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <EventCore.h>

TEST_CASE("fwEvent without callbacks is falsy and invoking it succeeds")
{
	fwEvent<> event;

	REQUIRE(!event);
	REQUIRE(event() == true);
}

TEST_CASE("fwEvent with a callback is truthy")
{
	fwEvent<> event;
	event.Connect([]() {});

	REQUIRE(static_cast<bool>(event));
}

TEST_CASE("fwEvent runs callbacks sorted by order, keeping connection order for equal orders")
{
	fwEvent<> event;
	std::vector<std::string> calls;

	event.Connect([&calls]() { calls.push_back("0"); }, 0);
	event.Connect([&calls]() { calls.push_back("5a"); }, 5);
	event.Connect([&calls]() { calls.push_back("-1"); }, -1);
	event.Connect([&calls]() { calls.push_back("5b"); }, 5);

	REQUIRE(event() == true);
	REQUIRE(calls == std::vector<std::string>{ "-1", "0", "5a", "5b" });
}

TEST_CASE("fwEvent stops the chain when a callback returns false")
{
	fwEvent<> event;
	std::vector<int> calls;

	event.Connect([&calls]() { calls.push_back(1); return true; }, 0);
	event.Connect([&calls]() { calls.push_back(2); return false; }, 1);
	event.Connect([&calls]() { calls.push_back(3); return true; }, 2);

	REQUIRE(event() == false);
	REQUIRE(calls == std::vector<int>{ 1, 2 });
}

TEST_CASE("fwEvent treats void callbacks as returning true")
{
	fwEvent<> event;
	int count = 0;

	event.Connect([&count]() { count++; }, 0);
	event.Connect([&count]() { count++; }, 1);

	REQUIRE(event() == true);
	REQUIRE(count == 2);
}

TEST_CASE("fwEvent Disconnect removes only the given callback")
{
	fwEvent<> event;
	std::vector<int> calls;

	auto a = event.Connect([&calls]() { calls.push_back(1); });
	auto b = event.Connect([&calls]() { calls.push_back(2); });
	event.Connect([&calls]() { calls.push_back(3); });

	event.Disconnect(b);
	event();
	REQUIRE(calls == std::vector<int>{ 1, 3 });

	calls.clear();
	event.Disconnect(a);
	event();
	REQUIRE(calls == std::vector<int>{ 3 });
}

TEST_CASE("fwEvent Disconnect of an already removed cookie does nothing")
{
	fwEvent<> event;
	int count = 0;

	auto cookie = event.Connect([&count]() { count++; });
	event.Connect([&count]() { count++; });

	event.Disconnect(cookie);
	event.Disconnect(cookie);
	event();

	REQUIRE(count == 1);
}

TEST_CASE("fwEvent Disconnect of the invalid cookie is a no-op")
{
	fwEvent<> event;
	int count = 0;

	event.Connect([&count]() { count++; });
	event.Disconnect(static_cast<size_t>(-1));
	event();

	REQUIRE(count == 1);
}

TEST_CASE("fwEvent becomes falsy after its only callback is disconnected")
{
	fwEvent<> event;

	auto cookie = event.Connect([]() {});
	event.Disconnect(cookie);

	REQUIRE(!event);
	REQUIRE(event() == true);
}

TEST_CASE("fwEvent rejects an empty std::function and hands out distinct cookies")
{
	fwEvent<> event;

	REQUIRE(event.Connect(std::function<bool()>{}) == static_cast<size_t>(-1));
	REQUIRE(!event);

	auto a = event.Connect([]() {});
	auto b = event.Connect([]() {});
	auto c = event.Connect([]() { return true; });

	REQUIRE(a != static_cast<size_t>(-1));
	REQUIRE(a != b);
	REQUIRE(b != c);
	REQUIRE(a != c);
}

TEST_CASE("fwEvent Reset removes every callback")
{
	fwEvent<> event;
	int count = 0;

	event.Connect([&count]() { count++; });
	event.Connect([&count]() { count++; }, 3);
	event.Reset();

	REQUIRE(!event);
	REQUIRE(event() == true);
	REQUIRE(count == 0);
}

TEST_CASE("fwEvent passes arguments through to callbacks")
{
	fwEvent<int, const std::string&> event;
	std::vector<std::string> calls;

	event.Connect([&calls](int value, const std::string& text)
	{
		calls.push_back("void:" + std::to_string(value) + ":" + text);
	});

	event.Connect([&calls](int value, const std::string& text)
	{
		calls.push_back("bool:" + std::to_string(value) + ":" + text);
		return true;
	},
	1);

	REQUIRE(event(42, std::string("hello")) == true);
	REQUIRE(calls == std::vector<std::string>{ "void:42:hello", "bool:42:hello" });
}

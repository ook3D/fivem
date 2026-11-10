#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <NetAddress.h>

static boost::optional<net::PeerAddress> Parse(const std::string& str, int defaultPort = 30120)
{
	return net::PeerAddress::FromString(str, defaultPort, net::PeerAddress::LookupType::NoResolution);
}

TEST_CASE("PeerAddress parses an IPv4 address with a port")
{
	auto addr = Parse("1.2.3.4:5000");

	REQUIRE(addr.has_value());
	REQUIRE(addr->GetAddressFamily() == AF_INET);
	REQUIRE(addr->GetHost() == "1.2.3.4");
	REQUIRE(addr->GetPort() == 5000);
	REQUIRE(addr->ToString() == "1.2.3.4:5000");
}

TEST_CASE("PeerAddress uses the default port when none is given")
{
	REQUIRE(Parse("1.2.3.4")->GetPort() == 30120);
	REQUIRE(Parse("1.2.3.4", 1234)->GetPort() == 1234);
}

TEST_CASE("PeerAddress parses a bracketed IPv6 address with a port")
{
	auto addr = Parse("[::1]:80");

	REQUIRE(addr.has_value());
	REQUIRE(addr->GetAddressFamily() == AF_INET6);
	REQUIRE(addr->GetHost() == "[::1]");
	REQUIRE(addr->GetPort() == 80);
	REQUIRE(addr->ToString() == "[::1]:80");
}

TEST_CASE("PeerAddress treats a bracketed IPv6 address without a port as using the default port")
{
	auto addr = Parse("[::1]");

	REQUIRE(addr.has_value());
	REQUIRE(addr->GetHost() == "[::1]");
	REQUIRE(addr->GetPort() == 30120);
}

TEST_CASE("PeerAddress rejects invalid input without resolution")
{
	REQUIRE(!Parse("not an ip").has_value());
	REQUIRE(!Parse("1.2.3.4.5").has_value());
	REQUIRE(!Parse("").has_value());
}

TEST_CASE("PeerAddress exposes host bytes for IPv4")
{
	auto addr = Parse("1.2.3.4:5000");
	REQUIRE(addr.has_value());

	auto bytes = addr->GetHostBytes();
	REQUIRE(bytes[0] == 1);
	REQUIRE(bytes[1] == 2);
	REQUIRE(bytes[2] == 3);
	REQUIRE(bytes[3] == 4);

	for (size_t i = 4; i < bytes.size(); i++)
	{
		REQUIRE(bytes[i] == 0);
	}
}

TEST_CASE("PeerAddress maps IPv4 to IPv6 in GetHostBytesV6")
{
	auto addr = Parse("1.2.3.4:5000");
	REQUIRE(addr.has_value());

	auto bytes = addr->GetHostBytesV6();

	for (size_t i = 0; i < 10; i++)
	{
		REQUIRE(bytes[i] == 0);
	}

	REQUIRE(bytes[10] == 0xFF);
	REQUIRE(bytes[11] == 0xFF);
	REQUIRE(bytes[12] == 1);
	REQUIRE(bytes[13] == 2);
	REQUIRE(bytes[14] == 3);
	REQUIRE(bytes[15] == 4);
}

TEST_CASE("PeerAddress exposes host bytes for IPv6")
{
	auto addr = Parse("[::1]:80");
	REQUIRE(addr.has_value());

	auto bytes = addr->GetHostBytes();

	for (size_t i = 0; i < 15; i++)
	{
		REQUIRE(bytes[i] == 0);
	}

	REQUIRE(bytes[15] == 1);
	REQUIRE(addr->GetHostBytesV6() == bytes);
}

TEST_CASE("PeerAddress comparison operators are consistent")
{
	auto a = Parse("1.2.3.4:5000");
	auto b = Parse("1.2.3.4:5000");
	auto c = Parse("1.2.3.4:5001");
	auto d = Parse("1.2.3.5:5000");

	REQUIRE(a.has_value());
	REQUIRE(b.has_value());
	REQUIRE(c.has_value());
	REQUIRE(d.has_value());

	REQUIRE(*a == *b);
	REQUIRE(!(*a != *b));
	REQUIRE(!(*a < *b));
	REQUIRE(*a != *c);
	REQUIRE(*a != *d);

	REQUIRE((*a < *c) != (*c < *a));
	REQUIRE((*a < *d) != (*d < *a));
}

TEST_CASE("Default constructed PeerAddress is empty")
{
	net::PeerAddress addr;

	REQUIRE(addr.GetPort() == 0);
	REQUIRE(addr.GetHost() == "0.0.0.0");
	REQUIRE(addr.GetAddressFamily() == 0);
}

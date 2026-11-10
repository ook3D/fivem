#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <TokenBucket.h>

TEST_CASE("Token bucket rejects consumption at or before its zero time")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE(bucket.consume(1, 0.0) == false);
	REQUIRE(bucket.available(0.0) == 0.0);
}

TEST_CASE("Token bucket starts full and consumption reduces available tokens")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE(bucket.available(1.0) == Catch::Approx(5));
	REQUIRE(bucket.consume(1, 1.0) == true);
	REQUIRE(bucket.available(1.0) == Catch::Approx(4));
}

TEST_CASE("Token bucket never holds more than its burst size")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE(bucket.available(1000.0) == Catch::Approx(5));
	REQUIRE(bucket.consume(6, 1000.0) == false);
	REQUIRE(bucket.available(1000.0) == Catch::Approx(5));
	REQUIRE(bucket.consume(5, 1000.0) == true);
}

TEST_CASE("Token bucket refills at the generation rate")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE(bucket.consume(5, 1.0) == true);
	REQUIRE(bucket.available(1.0) == Catch::Approx(0).margin(1e-9));
	REQUIRE(bucket.available(1.2) == Catch::Approx(2));
	REQUIRE(bucket.available(1.4) == Catch::Approx(4));
	REQUIRE(bucket.available(5.0) == Catch::Approx(5));
}

TEST_CASE("Token bucket consumeOrDrain takes what is available")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE(bucket.consumeOrDrain(10, 1.0) == Catch::Approx(5));
	REQUIRE(bucket.consumeOrDrain(10, 1.0) == 0.0);
	REQUIRE(bucket.consumeOrDrain(1, 1.5) == Catch::Approx(1));
}

TEST_CASE("Token bucket consumeWithBorrowNonBlocking reports the wait for borrowed tokens")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE_FALSE(bucket.consumeWithBorrowNonBlocking(6, 1.0).has_value());

	auto immediate = bucket.consumeWithBorrowNonBlocking(5, 1.0);
	REQUIRE(immediate.has_value());
	REQUIRE(*immediate == 0.0);

	auto borrowed = bucket.consumeWithBorrowNonBlocking(1, 1.0);
	REQUIRE(borrowed.has_value());
	REQUIRE(*borrowed == Catch::Approx(0.1));

	REQUIRE(bucket.consume(1, 1.05) == false);
}

TEST_CASE("Token bucket returnTokens re-credits tokens up to the burst size")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE(bucket.consume(5, 1.0) == true);
	bucket.returnTokens(3);
	REQUIRE(bucket.available(1.0) == Catch::Approx(3));

	bucket.returnTokens(100);
	REQUIRE(bucket.available(1.0) == Catch::Approx(5));
}

TEST_CASE("Token bucket reset keeps the available token count")
{
	folly::TokenBucket bucket(10, 5);

	REQUIRE(bucket.consume(3, 1.0) == true);
	REQUIRE(bucket.available(1.0) == Catch::Approx(2));

	bucket.reset(20, 8, 1.0);

	REQUIRE(bucket.rate() == 20.0);
	REQUIRE(bucket.burst() == 8.0);
	REQUIRE(bucket.available(1.0) == Catch::Approx(2));
	REQUIRE(bucket.available(1.1) == Catch::Approx(4));
}

TEST_CASE("Dynamic token bucket applies the rate and burst given on each call")
{
	folly::DynamicTokenBucket bucket;

	REQUIRE(bucket.consume(1, 10, 5, 1.0) == true);
	REQUIRE(bucket.consume(3, 10, 5, 1.0) == true);
	REQUIRE(bucket.consume(2, 10, 5, 1.0) == false);

	REQUIRE(bucket.consume(9, 100, 50, 1.0) == true);
	REQUIRE(bucket.consume(2, 100, 50, 1.0) == false);
	REQUIRE(bucket.available(100, 50, 2.0) == Catch::Approx(50));
}

TEST_CASE("Dynamic token bucket caps tokens to the burst of the current call")
{
	folly::DynamicTokenBucket bucket;

	REQUIRE(bucket.consume(1, 100, 0.5, 1.0) == false);
	REQUIRE(bucket.consume(0.5, 100, 0.5, 1.0) == true);
}

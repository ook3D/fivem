#include <StdInc.h>

#include <catch_amalgamated.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

#include <FixedBuffer.h>

namespace
{
int g_derivedDestructed = 0;

struct FixedBufferBase
{
	virtual ~FixedBufferBase() = default;
	virtual int Value() const = 0;
};

struct FixedBufferDerived : FixedBufferBase
{
	FixedBufferDerived(int value, std::string label)
		: m_value(value), m_label(std::move(label))
	{
	}

	~FixedBufferDerived() override
	{
		++g_derivedDestructed;
	}

	int Value() const override
	{
		return m_value;
	}

	int m_value;
	std::string m_label;
};

using TestBuffer = fx::FixedBuffer<FixedBufferBase, 64>;
using AlignedBuffer = fx::FixedBuffer<FixedBufferBase, 64, 32>;
}

static_assert(!std::is_copy_constructible_v<TestBuffer>);
static_assert(!std::is_move_constructible_v<TestBuffer>);
static_assert(!std::is_copy_assignable_v<TestBuffer>);
static_assert(!std::is_move_assignable_v<TestBuffer>);

TEST_CASE("FixedBuffer has no base before construction")
{
	TestBuffer buffer;

	REQUIRE(buffer.GetBase() == nullptr);
}

TEST_CASE("FixedBuffer constructs a derived object with the given arguments")
{
	g_derivedDestructed = 0;

	TestBuffer buffer;
	auto* derived = buffer.Construct<FixedBufferDerived>(42, std::string("label"));

	REQUIRE(derived != nullptr);
	REQUIRE(buffer.GetBase() != nullptr);
	REQUIRE(buffer.GetBase() == static_cast<FixedBufferBase*>(derived));
	REQUIRE(buffer.GetBase()->Value() == 42);
	REQUIRE(derived->m_label == "label");
}

TEST_CASE("FixedBuffer destroys the derived object exactly once")
{
	g_derivedDestructed = 0;

	{
		TestBuffer buffer;
		buffer.Construct<FixedBufferDerived>(1, std::string());

		REQUIRE(g_derivedDestructed == 0);
	}

	REQUIRE(g_derivedDestructed == 1);
}

TEST_CASE("FixedBuffer destroys nothing when never constructed")
{
	g_derivedDestructed = 0;

	{
		TestBuffer buffer;
	}

	REQUIRE(g_derivedDestructed == 0);
}

TEST_CASE("FixedBuffer storage honors the requested alignment")
{
	AlignedBuffer buffer;
	buffer.Construct<FixedBufferDerived>(7, std::string());

	REQUIRE(reinterpret_cast<uintptr_t>(buffer.GetBase()) % 32 == 0);
	REQUIRE(buffer.GetBase()->Value() == 7);
}

#pragma once

#include <string>
using String = std::string;

#include <vector>

template<typename T>
using Vector = std::vector<T>;

#include <cstdint>
using u8 = std::uint8_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using s16 = std::int16_t;

template<typename A, typename B>
struct Pair
{
    A first;
    B second;
};

#include <chrono>
using Clock = std::chrono::high_resolution_clock;
using Stamp = decltype(Clock::now());
using Duration = std::chrono::duration<float>;



#pragma once

#include "Units.hpp"

#include <cstdint>
#include <iostream>
#include <chrono>
#ifdef __x86_64__
#include <x86intrin.h>
#endif

namespace mean {
// -------------------------------------------------------------------------------------
#ifdef __x86_64__
inline uint64_t readTSC() {
	const uint64_t tsc = __rdtsc();
	return tsc;
}
inline uint64_t readTSCfenced() {
   _mm_mfence();
	const uint64_t tsc = __rdtsc();
   _mm_mfence();
	return tsc;
}
#elif defined(__aarch64__)
inline uint64_t readTSC() {
	uint64_t cnt;
	asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
	return cnt;
}
inline uint64_t readTSCfenced() {
	uint64_t cnt;
	asm volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(cnt));
	return cnt;
}
#endif
inline uint64_t tscDifferenceNs(uint64_t end, uint64_t start) {
	return (end - start) * 0.25; // = 2 GHz
}
inline uint64_t tscDifferenceUs(uint64_t end, uint64_t start) {
	return tscDifferenceNs(end, start) / 1000;
}
inline uint64_t tscDifferenceMs(uint64_t end, uint64_t start) {
	return tscDifferenceNs(end, start) / 1000000;
}
inline uint64_t tscDifferenceS(uint64_t end, uint64_t start) {
	return tscDifferenceNs(end, start) / 1000000000ull;
}
inline uint64_t nsToTSC(uint64_t ns) {
	return ns* 4; // = 2 GHz
}
static auto _staticStartTsc = readTSC();
inline u64 nanoFromTsc(uint64_t tp) {
	return tscDifferenceNs(tp, _staticStartTsc); 
}
using TimePoint = std::chrono::time_point<std::chrono::high_resolution_clock>;
inline TimePoint getTimePoint() {
	return std::chrono::high_resolution_clock::now();	
}
inline uint64_t timePointDifference(TimePoint end, TimePoint start) {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}
inline uint64_t timePointDifferenceUs(TimePoint end, TimePoint start) {
	return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}
inline uint64_t timePointDifferenceMs(TimePoint end, TimePoint start) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}
static auto _staticStartTimingPoint = getTimePoint();
inline u64 nanoFromTimePoint(TimePoint tp) {
	return timePointDifference(tp, _staticStartTimingPoint); 
}
inline float getSeconds() {
	auto tp = getTimePoint();
	return nanoFromTimePoint(tp) * NANO; 
}
inline float getRoundSeconds() {
	static auto last = getTimePoint();
	auto now = getTimePoint();
	auto diff = timePointDifference(now, last) * NANO;
	last = now;
	return diff; 
}

// -------------------------------------------------------------------------------------
} // namespace mean
// -------------------------------------------------------------------------------------

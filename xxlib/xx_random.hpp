﻿#pragma once

namespace xx
{
	// 并不清掉 o 的数据
	inline Random::Random(Random && o)
		: Object((MemPool*)nullptr)
	{
		inext = o.inext;
		inextp = o.inextp;
		memcpy(SeedArray, o.SeedArray, sizeof(SeedArray));
	}

	inline Random::Random(int32_t seed)
		: Object((MemPool*)nullptr)
	{
		Init(seed);
	}

	inline void Random::Init(int32_t seed)
	{
		int32_t ii;
		int32_t mj, mk;

		//Initialize our Seed array.
		//This algorithm comes from Numerical Recipes in C (2nd Ed.)
		int32_t subtraction = (seed == std::numeric_limits<int32_t>::min()) ? std::numeric_limits<int32_t>::max() : std::abs(seed);
		mj = MSEED - subtraction;
		SeedArray[55] = mj;
		mk = 1;
		for (int32_t i = 1; i < 55; i++)
		{  //Apparently the range [1..55] is special (Knuth) and so we're wasting the 0'th position.
			ii = (21 * i) % 55;
			SeedArray[ii] = mk;
			mk = mj - mk;
			if (mk < 0) mk += MBIG;
			mj = SeedArray[ii];
		}
		for (int32_t k = 1; k < 5; k++)
		{
			for (int32_t i = 1; i < 56; i++)
			{
				SeedArray[i] -= SeedArray[1 + (i + 30) % 55];
				if (SeedArray[i] < 0) SeedArray[i] += MBIG;
			}
		}
		inext = 0;
		inextp = 21;
	}

	inline double Random::Sample()
	{
		//Including this division at the end gives us significantly improved
		//random number distribution.
		return (InternalSample() * (1.0 / MBIG));
	}

	inline int32_t Random::InternalSample()
	{
		int32_t retVal;
		int32_t locINext = inext;
		int32_t locINextp = inextp;

		if (++locINext >= 56) locINext = 1;
		if (++locINextp >= 56) locINextp = 1;

		retVal = SeedArray[locINext] - SeedArray[locINextp];

		if (retVal == MBIG) retVal--;
		if (retVal < 0) retVal += MBIG;

		SeedArray[locINext] = retVal;

		inext = locINext;
		inextp = locINextp;

		return retVal;
	}
	inline int32_t Random::Next()
	{
		return InternalSample();
	}

	inline double Random::GetSampleForLargeRange()
	{
		// The distribution of double value returned by Sample 
		// is not distributed well enough for a large range.
		// If we use Sample for a range [Int32.MinValue..Int32.MaxValue)
		// We will end up getting even numbers only.

		int32_t result = InternalSample();
		// Note we can't use addition here. The distribution will be bad if we do that.
		bool negative = (InternalSample() % 2 == 0) ? true : false;  // decide the sign based on second sample
		if (negative)
		{
			result = -result;
		}
		double d = result;
		d += (std::numeric_limits<int32_t>::max() - 1); // get a number in range [0 .. 2 * Int32MaxValue - 1)
		d /= 2 * (uint32_t)std::numeric_limits<int32_t>::max() - 1;
		return d;
	}


	inline int32_t Random::Next(int32_t minValue, int32_t maxValue)
	{
		assert(minValue <= maxValue);

		int64_t range = (int64_t)maxValue - minValue;
		if (range <= (int64_t)std::numeric_limits<int32_t>::max())
		{
			return ((int32_t)(Sample() * range) + minValue);
		}
		else
		{
			return (int32_t)((int64_t)(GetSampleForLargeRange() * range) + minValue);
		}
	}


	inline int32_t Random::Next(int32_t maxValue)
	{
		assert(maxValue >= 0);
		return (int32_t)(Sample() * maxValue);
	}


	inline double Random::NextDouble()
	{
		return Sample();
	}


	//inline void Random::NextBytes(BBuffer* buffer)
	//{
	//	for (uint32_t i = 0; i < buffer->dataLen; i++)
	//	{
	//		buffer->buf[i] = (uint8_t)(InternalSample() % (std::numeric_limits<uint8_t>::max() + 1));
	//	}
	//}

	inline double Random::NextDouble(double minValue, double maxValue)
	{
		if (minValue == maxValue || maxValue - minValue <= 0) return minValue;
		return minValue + (maxValue - minValue) * NextDouble();
	}



	inline Random::Random(BBuffer* bb)
		: Object((MemPool*)nullptr)
	{
		if (int r = FromBBuffer(*bb)) throw r;
	}


	inline void Random::ToBBuffer(BBuffer &bb) const
	{
		// data len = 2 int + int[56] = 4 * 58 = 232
		bb.Reserve(bb.dataLen + 232);
		memcpy(bb.buf + bb.dataLen, &inext, 232);
		bb.dataLen += 232;
	}

	inline int Random::FromBBuffer(BBuffer &bb)
	{
		if (bb.offset + 232 > bb.dataLen) return -1;
		memcpy(&inext, bb.buf + bb.offset, 232);
		bb.offset += 232;
		return 0;
	}



	inline void Random::ToString(String &s) const
	{
		s.Append("{ \"type\":\"Random\" }");
	}
}

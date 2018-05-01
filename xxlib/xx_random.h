﻿#pragma once

namespace xx
{
	// 从 .NET System.Random 翻写, 理论上讲相同种子能输出相同结果. 支持序列化.
	// 必须传入种子
	class Random : public Object
	{
		explicit Random(int32_t seed = 0);
		Random(Random && o);
		Random(Random const&) = delete;
		Random& operator=(Random const&) = delete;

		static constexpr int32_t MBIG = std::numeric_limits<int32_t>::max();
		static constexpr int32_t MSEED = 161803398;
		static constexpr int32_t MZ = 0;

		int32_t inext;
		int32_t inextp;
		int32_t SeedArray[56];

		void Init(int32_t seed = 0);

		/*====================================Sample====================================
		**Action: Return a new random number [0..1) and reSeed the Seed array.
		**Returns: A double [0..1)
		**Arguments: None
		**Exceptions: None
		==============================================================================*/
		inline double Sample();

		inline int32_t InternalSample();
		double GetSampleForLargeRange();

	public:
		/*=====================================Next=====================================
		**Returns: An int32_t [0..Int32.MaxValue)
		**Arguments: None
		**Exceptions: None.
		==============================================================================*/
		inline int32_t Next();

		/*=====================================Next=====================================
		**Returns: An int32_t [minvalue..maxvalue)
		**Arguments: minValue -- the least legal value for the Random number.
		**           maxValue -- One greater than the greatest legal return value.
		**Exceptions: None.
		==============================================================================*/
		inline int32_t Next(int32_t minValue, int32_t maxValue);


		/*=====================================Next=====================================
		**Returns: An int32_t [0..maxValue)
		**Arguments: maxValue -- One more than the greatest legal return value.
		**Exceptions: None.
		==============================================================================*/
		inline int32_t Next(int32_t maxValue);


		/*=====================================Next=====================================
		**Returns: A double [0..1)
		**Arguments: None
		**Exceptions: None
		==============================================================================*/
		inline double NextDouble();


		///*==================================NextBytes===================================
		//**Action:  Fills the byte array with random bytes [0..0x7f].  The entire array is filled.
		//**Returns:Void
		//**Arugments:  buffer -- the array to be filled.
		//**Exceptions: None
		//==============================================================================*/
		//void NextBytes(BBuffer* buffer);

		double NextDouble(double minValue, double maxValue);



		Random(BBuffer* bb);
		void ToBBuffer(BBuffer &bb) const override;
		int FromBBuffer(BBuffer &bb) override;

		void ToString(String &s) const override;
	};

	using Random_r = Ref<Random>;
	using Random_p = Ptr<Random>;
}

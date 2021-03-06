#pragma once
#include "xxlib_defines.h"
#include "xxlib_mpobject.h"
#include "xxlib_bytesutils.h"
#include "xxlib_mempool.h"
#include <cassert>

namespace xxlib
{
	// 类似 std vector / .net List 的简化容器
	// autoRelease 意思是当目标类型为 MPObject* 派生时, 构造时将 call 其 AddRef, 析构时将 call 其 Release
	// reservedHeaderLen 为分配 buf 内存后在前面空出一段内存不用, 也不初始化, 扩容不复制( 为附加头部数据创造便利 )
	template<typename T, bool autoRelease = false, uint32_t reservedHeaderLen = 0>
	struct List : public MPObject
	{
		static_assert(!autoRelease || (std::is_pointer<T>::value && IsMPObject<T>::value), "autoRelease == true cond is T* where T : MPObject");

		typedef T ChildType;
		T*			buf;
		uint32_t    bufLen;
		uint32_t    dataLen;

	protected:
		friend MemPool;
		explicit List(uint32_t capacity = 0)
		{
			if (capacity == 0)
			{
				buf = nullptr;
				bufLen = 0;
			}
			else
			{
				auto bufByteLen = Round2n((reservedHeaderLen + capacity) * sizeof(T) + 8) - 8;
				buf = (T*)mempool().Alloc((uint32_t)bufByteLen) + reservedHeaderLen;
				bufLen = uint32_t(bufByteLen / sizeof(T)) - reservedHeaderLen;
			}
			dataLen = 0;
		}
		List(MemPool&) : List(0) {}		// for 反序列化
		~List()
		{
			Clear(true);
		}
		List(List const&o) = delete;
		List& operator=(List const&o) = delete;

		template<typename U = T> void ItemAddRef(std::enable_if_t<!autoRelease, U> &p) {}
		template<typename U = T> void ItemAddRef(std::enable_if_t< autoRelease, U> &p) { if (p) p->AddRef(); }
		template<typename U = T> void ItemRelease(std::enable_if_t<!autoRelease, U> &p) {}
		template<typename U = T> void ItemRelease(std::enable_if_t< autoRelease, U> &p) { if (p) p->Release(); }
	public:

		void Reserve(uint32_t capacity)
		{
			if (capacity <= bufLen) return;

			auto newBufByteLen = Round2n(reservedHeaderLen + capacity * sizeof(T) + 8) - 8;
			auto newBuf = (T*)mempool().Alloc((uint32_t)newBufByteLen) + reservedHeaderLen;

			if (std::is_trivial<T>::value || MemmoveSupport<T>::value)
			{
				memcpy(newBuf, buf, dataLen * sizeof(T));
			}
			else
			{
				for (uint32_t i = 0; i < dataLen; ++i)
				{
					new (&newBuf[i]) T((T&&)buf[i]);
					buf[i].~T();
				}
			}
			// memcpy(newBuf - reservedHeaderLen, buf - reservedHeaderLen, reservedHeaderLen * sizeof(T));

			if (buf) mempool().Free(buf - reservedHeaderLen);
			buf = newBuf;
			bufLen = uint32_t(newBufByteLen / sizeof(T)) - reservedHeaderLen;
		}

		// 返回扩容前的长度
		uint32_t Resize(uint32_t len)
		{
			if (len == dataLen) return dataLen;
			else if (len < dataLen && !std::is_trivial<T>::value)
			{
				for (uint32_t i = len; i < dataLen; ++i)
				{
					buf[i].~T();
				}
			}
			else // len > dataLen
			{
				Reserve(len);
				if (!std::is_pod<T>::value)
				{
					for (uint32_t i = dataLen; i < len; ++i)
					{
						new (buf + i) T();
					}
				}
			}
			auto rtv = dataLen;
			dataLen = len;
			return rtv;
		}

		T const& operator[](uint32_t idx) const
		{
			assert(idx < dataLen);
			return buf[idx];
		}
		T& operator[](uint32_t idx)
		{
			assert(idx < dataLen);
			return buf[idx];
		}
		T const& At(uint32_t idx) const
		{
			assert(idx < dataLen);
			return buf[idx];
		}
		T& At(uint32_t idx)
		{
			assert(idx < dataLen);
			return buf[idx];
		}

		uint32_t Count() const
		{
			return dataLen;
		}

		T& Top()
		{
			assert(dataLen > 0);
			return buf[dataLen - 1];
		}
		void Pop()
		{
			assert(dataLen > 0);
			--dataLen;
			ItemRelease(buf[dataLen]);
			buf[dataLen].~T();
		}
		T const& Top() const
		{
			assert(dataLen > 0);
			return buf[dataLen - 1];
		}
		bool TryPop(T& output)
		{
			if (!dataLen) return false;
			output = (T&&)buf[--dataLen];
			ItemRelease(buf[dataLen]);
			buf[dataLen].~T();
			return true;
		}

		void Clear(bool freeBuf = false)
		{
			if (!buf) return;
			for (uint32_t i = 0; i < dataLen; ++i)
			{
				ItemRelease(buf[i]);
				buf[i].~T();
			}
			dataLen = 0;
			if (freeBuf)
			{
				mempool().Free(buf - reservedHeaderLen);
				buf = nullptr;
				bufLen = 0;
			}
		}

		// 移除指定索引的元素. 为紧凑排列, 可能产生内存移动
		void RemoveAt(uint32_t idx)
		{
			assert(idx < dataLen);
			--dataLen;
			if (std::is_trivial<T>::value || MemmoveSupport<T>::value)
			{
				ItemRelease(buf[idx]);
				buf[idx].~T();
				memmove(buf + idx, buf + idx + 1, (dataLen - idx) * sizeof(T));
			}
			else
			{
				for (uint32_t i = idx; i < dataLen; ++i)
				{
					buf[i] = (T&&)buf[i + 1];
				}
				buf[dataLen].~T();
			}
		}

		// 遍历查找并移除
		void Remove(T const& v)
		{
			for (uint32_t i = 0; i < dataLen; ++i)
			{
				if (EqualsTo(v, buf[i]))
				{
					RemoveAt(i);
					return;
				}
			}
		}

		// 交换移除法, 返回是否产生过交换行为
		bool SwapRemoveAt(uint32_t idx)
		{
			assert(idx < dataLen);

			auto& o = buf[idx];
			ItemRelease(o);
			o.~T();

			--dataLen;
			if (dataLen == idx) return false;	// last one
			else if (std::is_trivial<T>::value || MemmoveSupport<T>::value)
			{
				o = buf[dataLen];
			}
			else
			{
				new (&o) T((T&&)buf[dataLen]);
				buf[dataLen].~T();
			}
			return true;
		}

	protected:
		// 不越界检查 不加持
		void FastAddDirect(T const& v)
		{
			new (buf + dataLen) T(v);
			++dataLen;
		}

		// 不越界检查( 右值版 ) 不加持
		void FastAddDirect(T&& v)
		{
			new (buf + dataLen) T((T&&)v);
			++dataLen;
		}

		// 不越界检查
		void FastAdd(T const& v)
		{
			new (buf + dataLen) T(v);
			ItemAddRef(buf[dataLen]);
			++dataLen;
		}

		// 不越界检查( 右值版 )
		void FastAdd(T&& v)
		{
			new (buf + dataLen) T((T&&)v);
			ItemAddRef(buf[dataLen]);
			++dataLen;
		}

	public:

		// 移动 / 复制添加一到多个( 不加持 )
		template<typename...VS>
		void AddMultiDirect(VS&&...vs)
		{
			static_assert(sizeof...(vs), "lost args ??");
			Reserve(dataLen + sizeof...(vs));
			std::initializer_list<int>{ (FastAddDirect(std::forward<VS>(vs)), 0)... };
		}

		// 移动 / 复制添加一到多个
		template<typename...VS>
		void AddMulti(VS&&...vs)
		{
			static_assert(sizeof...(vs), "lost args ??");
			Reserve(dataLen + sizeof...(vs));
			std::initializer_list<int>{ (FastAdd(std::forward<VS>(vs)), 0)... };
		}

		// 追加一个 item( 右值版 )
		void Add(T&& v)
		{
			Emplace((T&&)v);
		}
		// 追加一个 item
		void Add(T const& v)
		{
			Emplace(v);
		}

		// 追加一个 item( 右值版 ). 不加持
		void AddDirect(T&& v)
		{
			EmplaceDirect((T&&)v);
		}
		// 追加一个 item. 不加持
		void AddDirect(T const& v)
		{
			EmplaceDirect(v);
		}

		// 用参数直接构造一个( 当前不支持直接经由 mempool Create MPObject* ) 不加持
		template<typename...Args>
		T& EmplaceDirect(Args &&... args)
		{
			Reserve(dataLen + 1);
			auto& p = buf[dataLen++];
			new (&p) T(std::forward<Args>(args)...);
			return p;
		}

		// 用参数直接构造一个( 当前不支持直接经由 mempool Create MPObject* )
		template<typename...Args>
		T& Emplace(Args &&... args)
		{
			auto& p = EmplaceDirect(std::forward<Args>(args)...);
			ItemAddRef(p);
			return p;
		}

		void InsertAt(uint32_t idx, T&& v)
		{
			EmplaceAt(idx, (T&&)v);
		}
		void InsertAt(uint32_t idx, T const& v)
		{
			EmplaceAt(idx, v);
		}

		// 用参数直接构造一个到指定位置( 当前不支持经由 mempool 创建 MPObject* )
		template<typename...Args>
		T& EmplaceAt(uint32_t idx, Args&&...args)
		{
			Reserve(dataLen + 1);
			if (idx < dataLen)
			{
				if (std::is_trivial<T>::value || MemmoveSupport<T>::value)
				{
					memmove(buf + idx + 1, buf + idx, (dataLen - idx) * sizeof(T));
				}
				else
				{
					new (buf + dataLen) T((T&&)buf[dataLen - 1]);
					for (uint32_t i = dataLen - 1; i > idx; --i)
					{
						buf[i] = (T&&)buf[i - 1];
					}
					buf[idx].~T();
				}
			}
			else idx = dataLen;
			++dataLen;
			new (buf + idx) T(std::forward<Args>(args)...);
			ItemAddRef(buf[idx]);
			return buf[idx];
		}
		void AddRange(T const* items, uint32_t count)
		{
			Reserve(dataLen + count);
			if (std::is_trivial<T>::value || MemmoveSupport<T>::value)
			{
				memcpy(buf, items, count * sizeof(T));
			}
			else
			{
				for (uint32_t i = 0; i < count; ++i)
				{
					new (&buf[dataLen + i]) T((T&&)items[i]);
				}
			}
			for (uint32_t i = 0; i < count; ++i)
			{
				ItemAddRef(buf[dataLen + i]);
			}
			dataLen += count;
		}



		// 如果找到就返回索引. 找不到将返回 uint32_t(-1)
		uint32_t Find(T const& v)
		{
			for (uint32_t i = 0; i < dataLen; ++i)
			{
				if (EqualsTo(v, buf[i])) return i;
			}
			return uint32_t(-1);
		}

		// 如果找到就返回索引. 找不到将返回 uint32_t(-1)
		uint32_t Find(std::function<bool(T&)> cond)
		{
			for (uint32_t i = 0; i < dataLen; ++i)
			{
				if (cond(buf[i])) return i;
			}
			return uint32_t(-1);
		}

		// 返回是否存在
		bool Exists(std::function<bool(T&)> cond)
		{
			return Find(cond) != uint32_t(-1);
		}

		bool TryFill(T& out, std::function<bool(T&)> cond)
		{
			auto idx = Find(cond);
			if (idx == uint32_t(-1)) return false;
			out = buf[idx];
			return true;
		}


		// handler 返回 false 则 foreach 终止
		void ForEach(std::function<bool(T&)> handler)
		{
			for (uint32_t i = 0; i < dataLen; ++i)
			{
				if (handler(buf[i])) return;
			}
		}




	public:
		// 只是为了能 for( auto c : 
		struct Iter
		{
			T *ptr;
			bool operator!=(Iter const& other) { return ptr != other.ptr; }
			Iter& operator++()
			{
				++ptr;
				return *this;
			}
			T& operator*() { return *ptr; }
		};
		Iter begin()
		{
			return Iter{ buf };
		}
		Iter end()
		{
			return Iter{ buf + dataLen };
		}
		Iter begin() const
		{
			return Iter{ buf };
		}
		Iter end() const
		{
			return Iter{ buf + dataLen };
		}


		/*************************************************************************/
		// 实现 ToString 接口
		/*************************************************************************/

		virtual void ToString(String &str) const override;


		/**************************************************************************************************************/
		// 序列化相关
		/**************************************************************************************************************/

		// 写
		virtual void ToBBuffer(BBuffer &bb) const override
		{
			ToByteBufferCore(bb);
		}

	protected:
		template<typename U = BBuffer>
		void ToByteBufferCore(std::enable_if_t< IsMPObject<T>::value, U> &bb) const
		{
			bb.Write(dataLen);
			for (uint32_t i = 0; i < dataLen; ++i) bb.Write(buf[i]);
		}

		template<typename U = BBuffer>
		void ToByteBufferCore(std::enable_if_t<!IsMPObject<T>::value, U > &bb) const
		{
			bb.Reserve(bb.dataLen + WriteToBBCalc<T>(dataLen));
			WriteToBB(bb.buf + bb.dataLen, buf, dataLen);
			assert(bb.dataLen <= bb.bufLen);
		}
	public:

		// 读
		virtual int FromBBuffer(BBuffer &bb) override
		{
			return FromBBufferCore(bb);
		}

	protected:
		template<typename U = BBuffer>
		int FromBBufferCore(std::enable_if_t< IsMPObject<T>::value, U> &bb)
		{
			int rtv = 0;
			uint32_t len;
			if ((rtv = bb.Read(len))) return rtv;
			Clear();
			Reserve(len);
			for (uint32_t i = 0; i < len; i++)
			{
				if ((rtv = bb.Read(buf[dataLen]))) goto LabRelease;	// 中途读失败跳转到回收代码
				++dataLen;
			}
			return 0;
		LabRelease:
			for (uint32_t i = 0; i < dataLen; i++)
			{
				if (buf[i]) buf[i]->Release();
			}
			dataLen = 0;
			return rtv;
		}
		template<typename U = BBuffer>
		int FromBBufferCore(std::enable_if_t<!IsMPObject<T>::value, U> &bb)
		{
			return ReadFromBB(bb.buf + bb.offset, bb.dataLen, bb.offset, *this);
		}
	public:

		/****************************************************************************************************/
		// 下列函数仅针对值类型
		/****************************************************************************************************/
	protected:

		// 填充长度估算
		template<typename VT>
		static uint32_t WriteToBBCalc(uint32_t dataLen)
		{
			return 5 + dataLen * sizeof(VT) + (sizeof(VT) <= 2 || std::is_same<VT, float>::value ? 0 : 1);		// 变长要比 sizeof 多 1
		}

		template<typename VT>
		static uint32_t WriteToBB(char *dstBuf, VT* const &in, uint32_t const& dataLen)
		{
			auto writeOffset = BBWriteTo(dstBuf, dataLen);								// 写长度
			if (sizeof(VT) <= 2 || std::is_same<VT, float>::value)
			{
				memcpy(dstBuf + writeOffset, in, dataLen * sizeof(VT));					// 定长直接复制内存
				writeOffset += dataLen * sizeof(VT);
			}
			else
			{
				for (uint32_t i = 0; i < dataLen; ++i)
				{
					writeOffset += BBWriteTo(dstBuf + writeOffset, in[i]);				// 变长逐个写
				}
			}
			return writeOffset;		// 返回写了多长
		}

		template<typename VT, bool AR, uint32_t HS>
		static int ReadFromBB(char const *srcBuf, uint32_t const &dataLen, uint32_t &offset, List<VT, AR, HS> &out)
		{
			uint32_t len;
			auto rtv = BBReadFrom(srcBuf, dataLen, offset, len);					// 读长度
			if (!rtv) return rtv;
			//if (len < minLen || (maxLen > 0 && len > maxLen)) return -5;			// todo: 从长度限制映射数组读值

			out.Clear();
			if (!len) return 0;

			if (sizeof(VT) <= 2 || std::is_same<VT, float>::value)
			{
				if (offset + len * sizeof(VT) > dataLen) return -1;
				out.Resize(len);
				memcpy(out.buf, srcBuf + offset, len * sizeof(VT));
			}
			else
			{
				out.Resize(len);
				for (uint32_t i = 0; i < len; ++i)
				{
					auto rtv = BBReadFrom(srcBuf, dataLen, offset, out[i]);
					if (rtv)
					{
						out.Clear();
						return rtv;
					}
				}
			}
			return 0;
		}

	};

}

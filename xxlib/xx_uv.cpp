﻿#define _CRT_SECURE_NO_WARNINGS
#include <uv.h>
#include "ikcp.h"
#include "xx_uv.h"

static void* Alloc(xx::MemPool* const& mp, size_t const& size, void* const& ud)
{
	auto p = (void**)mp->Alloc(size + sizeof(void*));
	p[0] = ud;
	return &p[1];
}

static void* Alloc(xx::MemPool* const& mp, size_t const& size)
{
	return (void**)mp->Alloc(size + sizeof(void*)) + 1;
}

static void Free(xx::MemPool* const& mp, void* const& p) noexcept
{
	mp->Free((void**)p - 1);
}

static void Close(uv_handle_t* const& p) noexcept
{
	if (uv_is_closing(p)) return;
	uv_close(p, [](uv_handle_t* h)
	{
		Free((xx::MemPool*)h->loop->data, h);
	});
}

static void AllocCB(uv_handle_t* h, size_t suggested_size, uv_buf_t* buf)
{
	buf->base = (char*)((xx::MemPool*)h->loop->data)->Alloc(suggested_size);
	buf->len = decltype(buf->len)(suggested_size);
}

static int TcpWrite(void* stream, char* inBuf, uint32_t len) noexcept
{
	struct write_req_t
	{
		xx::MemPool* mp;
		uv_write_t req;
		uv_buf_t buf;
	};
	auto mp = (xx::MemPool*)((uv_stream_t*)stream)->loop->data;
	auto req = (write_req_t*)mp->Alloc(sizeof(write_req_t));
	req->mp = mp;
	auto buf = (char*)mp->Alloc(len);
	memcpy(buf, inBuf, len);
	req->buf = uv_buf_init(buf, (uint32_t)len);
	return uv_write((uv_write_t*)req, (uv_stream_t*)stream, &req->buf, 1, [](uv_write_t *req, int status)
	{
		//if (status) fprintf(stderr, "Write error: %s\n", uv_strerror(status));
		auto *wr = (write_req_t*)req;
		wr->mp->Free(wr->buf.base);
		wr->mp->Free(wr);
	});
}

static int FillIP(uv_tcp_t* stream, char* buf, size_t bufLen)
{
	sockaddr_in saddr;
	int len = sizeof(saddr);
	int r = 0;
	if ((r = uv_tcp_getpeername(stream, (sockaddr*)&saddr, &len))) return r;
	if ((r = uv_ip4_name(&saddr, buf, (int)bufLen))) throw r;
	auto dataLen = strlen(buf);
	return r;
}

static int FillIP(sockaddr_in* addr, char* buf, size_t bufLen)
{
	int r = 0;
	if ((r = uv_ip4_name(addr, buf, (int)bufLen))) throw r;
	auto dataLen = strlen(buf);
	sprintf(buf + dataLen, ":%d", ntohs(addr->sin_port));
	return r;
}


xx::UvLoop::UvLoop(MemPool* const& mp)
	: Object(mp)
	, tcpListeners(mp)
	, tcpClients(mp)
	, udpListeners(mp)
	, udpClients(mp)
	, timers(mp)
	, asyncs(mp)
{
	ptr = Alloc(mp, sizeof(uv_loop_t), this);
	if (!ptr) throw - 1;

	if (int r = uv_loop_init((uv_loop_t*)ptr))
	{
		Free(mp, ptr);
		ptr = nullptr;
		throw r;
	}
	((uv_loop_t*)ptr)->data = mp;

	// 感觉启用内存池之后似乎没啥作用, cpu 占用反而更高了
	// kcp 这个设计不科学. 直接就是全局改了. 这里反复改也不会出事
	//ikcp_allocator([](auto a, auto s)
	//{
	//	return ((xx::MemPool*)a)->Alloc(s);
	//}, [](auto a, auto p)
	//{
	//	((xx::MemPool*)a)->Free(p);
	//});
}

xx::UvLoop::~UvLoop()
{
	assert(ptr);

	udpListeners.ForEachRevert([mp = this->mempool](auto& o) { mp->Release(o); });
	udpClients.ForEachRevert([&mp = this->mempool](auto& o) { mp->Release(o); });
	tcpListeners.ForEachRevert([&mp = this->mempool](auto& o) { mp->Release(o); });
	tcpClients.ForEachRevert([&mp = this->mempool](auto& o) { mp->Release(o); });
	mempool->Release(udpTimer); udpTimer = nullptr;
	mempool->Release(timeouter); timeouter = nullptr;
	mempool->Release(rpcMgr);  rpcMgr = nullptr;
	timers.ForEachRevert([&mp = this->mempool](auto& o) { mp->Release(o); });
	asyncs.ForEachRevert([&mp = this->mempool](auto& o) { mp->Release(o); });

	if (uv_loop_close((uv_loop_t*)ptr))
	{
		uv_run((uv_loop_t*)ptr, UV_RUN_DEFAULT);
		uv_loop_close((uv_loop_t*)ptr);
	}
	Free(mempool, ptr);
	ptr = nullptr;
}

void xx::UvLoop::InitTimeouter(uint64_t const& intervalMS, int const& wheelLen, int const& defaultInterval)
{
	if (timeouter) throw - 1;
	mempool->CreateTo(timeouter, *this, intervalMS, wheelLen, defaultInterval);
}
void xx::UvLoop::InitRpcManager(uint64_t const& rpcIntervalMS, int const& rpcDefaultInterval)
{
	if (rpcMgr) throw - 1;
	mempool->CreateTo(rpcMgr, *this, rpcIntervalMS, rpcDefaultInterval);
}
void xx::UvLoop::InitKcpFlushInterval(uint32_t const& interval)
{
	if (udpTimer) throw - 1;
	kcpInterval = interval;
	mempool->CreateTo(udpTimer, *this, 0, interval, [this]
	{
		udpTicks += kcpInterval;
		for (auto& L : udpListeners)
		{
			for (auto& kv : L->peers)
			{
				kv.value->Update(udpTicks);
			}
		}
		for (int i = (int)udpClients.dataLen - 1; i >= 0; --i)
		{
			udpClients[i]->Update(udpTicks);
		}
	});
}

void xx::UvLoop::Run(UvRunMode const& mode)
{
	if (int r = uv_run((uv_loop_t*)ptr, (uv_run_mode)mode))
	{
		if (r != 1)
			throw r;	// ctrl break
	}
}

void xx::UvLoop::Stop()
{
	uv_stop((uv_loop_t*)ptr);
}

bool xx::UvLoop::alive() const
{
	return uv_loop_alive((uv_loop_t*)ptr) != 0;
}

xx::UvTcpListener* xx::UvLoop::CreateTcpListener()
{
	return mempool->Create<UvTcpListener>(*this);
}
xx::UvTcpClient* xx::UvLoop::CreateTcpClient()
{
	return mempool->Create<UvTcpClient>(*this);
}

xx::UvUdpListener* xx::UvLoop::CreateUdpListener()
{
	return mempool->Create<UvUdpListener>(*this);
}
xx::UvUdpClient* xx::UvLoop::CreateUdpClient()
{
	return mempool->Create<UvUdpClient>(*this);
}

xx::UvTimer* xx::UvLoop::CreateTimer(uint64_t const& timeoutMS, uint64_t const& repeatIntervalMS, std::function<void()>&& OnFire)
{
	return mempool->Create<UvTimer>(*this, timeoutMS, repeatIntervalMS, std::move(OnFire));
}
xx::UvAsync* xx::UvLoop::CreateAsync()
{
	return mempool->Create<UvAsync>(*this);
}









xx::UvListenerBase::UvListenerBase(UvLoop& loop)
	: Object(loop.mempool)
	, loop(loop)
{
}








xx::UvTcpListener::UvTcpListener(UvLoop& loop)
	: UvListenerBase(loop)
	, peers(loop.mempool)
{
	ptr = Alloc(mempool, sizeof(uv_tcp_t), this);
	if (!ptr) throw - 1;

	if (int r = uv_tcp_init((uv_loop_t*)loop.ptr, (uv_tcp_t*)ptr))
	{
		Free(mempool, ptr);
		ptr = nullptr;
	}

	addrPtr = Alloc(mempool, sizeof(sockaddr_in));
	if (!addrPtr)
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw - 1;
	}

	index_at_container = loop.tcpListeners.dataLen;
	loop.tcpListeners.Add(this);
}

xx::UvTcpListener::~UvTcpListener()
{
	assert(ptr);
	peers.ForEachRevert([mp = mempool](auto& o) { mp->Release(o); });
	Close((uv_handle_t*)ptr);
	ptr = nullptr;
	Free(mempool, addrPtr);
	addrPtr = nullptr;
}

void xx::UvTcpListener::OnAcceptCB(void* server, int status)
{
	if (status != 0) return;
	auto listener = *((UvTcpListener**)server - 1);
	UvTcpPeer* peer = nullptr;
	try
	{
		if (listener->OnCreatePeer) peer = listener->OnCreatePeer();
		else listener->loop.mempool->CreateTo(peer, *listener);
	}
	catch (...)
	{
		return;
	}
	if (peer && listener->OnAccept) listener->OnAccept(peer);
}

void xx::UvTcpListener::Bind(char const* const& ipv4, int const& port)
{
	if (int r = uv_ip4_addr(ipv4, port, (sockaddr_in*)addrPtr)) throw r;
	if (int r = uv_tcp_bind((uv_tcp_t*)ptr, (sockaddr*)addrPtr, 0)) throw r;
}

void xx::UvTcpListener::Listen(int const& backlog)
{
	if (int r = uv_listen((uv_stream_t*)ptr, backlog, (uv_connection_cb)OnAcceptCB)) throw r;
}










xx::UvTimeouterBase::UvTimeouterBase(MemPool* const& mp)
	: Object(mp)
{
}
xx::UvTimeouterBase::~UvTimeouterBase()
{
	UnbindTimeoutManager();
	OnTimeout = nullptr;
}

void xx::UvTimeouterBase::TimeouterClear()
{
	timeouterPrev = nullptr;
	timeouterNext = nullptr;
	timeouterIndex = -1;
}

void xx::UvTimeouterBase::TimeoutReset(int const& interval)
{
	if (!timeouter) throw - 1;
	timeouter->AddOrUpdate(this, interval);
}

void xx::UvTimeouterBase::TimeouterStop()
{
	if (!timeouter) throw - 1;
	if (timeouting()) timeouter->Remove(this);
}

void xx::UvTimeouterBase::BindTimeoutManager(UvTimeoutManager* const& t)
{
	if (timeouter) throw - 1;
	timeouter = t;
}

void xx::UvTimeouterBase::UnbindTimeoutManager()
{
	if (timeouting()) timeouter->Remove(this);
	timeouter = nullptr;
}

bool xx::UvTimeouterBase::timeouting()
{
	return timeouter && (timeouterIndex != -1 || timeouterPrev);
}








xx::UvTcpUdpBase::UvTcpUdpBase(UvLoop& loop)
	: UvTimeouterBase(loop.mempool)
	, routingAddress(loop.mempool)
	, senderAddress(loop.mempool)
	, loop(loop)
	, bbRecv(loop.mempool)
	, bbSend(loop.mempool)
{
}

void xx::UvTcpUdpBase::BindTimeoutManager(UvTimeoutManager* const& t)
{
	if (timeouter) throw - 1;
	timeouter = t ? t : loop.timeouter;
}

// 包头 = 1字节掩码 + 数据长(2/4字节) + 地址(转发) + 流水号(RPC)
// 1字节掩码:
// ......XX :   00: 一般数据包      01: RPC请求包       10: RPC回应包
// .....X.. :   0: 2字节数据长       1: 4字节数据长
// ....X... :   0: 包头中不带地址    1: 带地址(长度由 XXXX.... 部分决定, 值需要+1)
void xx::UvTcpUdpBase::ReceiveImpl(char const* const& bufPtr, int const& len)
{
	// 检测用户事件代码执行过后收包行为是否还该继续( 如果只是 client disconnect 则用 bbRecv.dataLen == 0 来检测 )
	auto versionNumber = memHeader().versionNumber;

	bbRecv.WriteBuf(bufPtr, len);					// 追加收到的数据到接收缓冲区

	auto buf = (uint8_t*)bbRecv.buf;				// 方便使用
	size_t offset = 0;								// 独立于 bbRecv 以避免受到其负面影响
	while (offset + 3 <= bbRecv.dataLen)			// 确保 3字节 包头长度
	{
		auto typeId = buf[offset];					// 读出头
		int pkgType = 0;							// 备用

		auto dataLen = (size_t)(buf[offset + 1] + (buf[offset + 2] << 8));
		int headerLen = 3;
		if ((typeId & 0x4) > 0)                     // 大包确保 5字节 包头长度
		{
			if (offset + 5 > bbRecv.dataLen) break;
			headerLen = 5;
			dataLen += (buf[offset + 3] << 16) + (buf[offset + 4] << 24);   // 修正为大包包长
		}


		if (dataLen <= 0)                           // 数据异常
		{
			DisconnectImpl();
			return;
		}
		if (offset + headerLen + dataLen > bbRecv.dataLen) break;   // 确保数据长
		auto pkgOffset = offset;
		offset += headerLen;

		size_t addrLen = 0;
		if ((typeId & 8) > 0)                       // 转发类数据包
		{
			addrLen = ((size_t)typeId >> 4) + 1;
			auto addrOffset = offset;
			auto pkgLen = offset + dataLen - pkgOffset;

			// 进一步判断是否有设置地址信息( 如果 routingAddress 非空, 表示当前连接为数据接收方, 否则就是路由 )
			if (routingAddress.dataLen)
			{
				// 存返件地址之后 跳到正常包逻辑代码继续处理
				senderAddress.Assign((char*)buf + addrOffset, addrLen);
				goto LabAfterAddress;
			}

			bbRecv.offset = offset - headerLen;
			if (OnReceiveRouting) OnReceiveRouting(bbRecv, pkgLen, addrOffset, addrLen);
			if (versionNumber != memHeader().versionNumber || !bbRecv.dataLen) return;
			if (Disconnected())
			{
				bbRecv.Clear();
				return;
			}

			// 路由服务直接全权处理, 不再继续抛出进阶事件, 故跳之
			goto LabEnd;
		}
		// 非转发包不含返回地址, 清空以便于在事件函数中判断来源( goto LabAfterAddress 的会跳过这步 )
		senderAddress.Clear();


	LabAfterAddress:
		bbRecv.offset = offset + addrLen;
		pkgType = typeId & 3;
		if (pkgType == 0)
		{
			if (OnReceivePackage) OnReceivePackage(bbRecv);
			if (versionNumber != memHeader().versionNumber || !bbRecv.dataLen) return;
			if (Disconnected())
			{
				bbRecv.Clear();
				return;
			}
		}
		else
		{
			uint32_t serial = 0;
			if (bbRecv.Read(serial))
			{
				DisconnectImpl();
				return;
			}
			if (pkgType == 1)
			{
				if (OnReceiveRequest) OnReceiveRequest(serial, bbRecv);
				if (versionNumber != memHeader().versionNumber || !bbRecv.dataLen) return;
				if (Disconnected())
				{
					bbRecv.Clear();
					return;
				}
			}
			else if (pkgType == 2)
			{
				loop.rpcMgr->Callback(serial, &bbRecv);
				if (versionNumber != memHeader().versionNumber || !bbRecv.dataLen) return;
				if (Disconnected())
				{
					bbRecv.Clear();
					return;
				}
			}
		}

	LabEnd:
		offset += dataLen;
	}
	if (offset < bbRecv.dataLen)
	{
		memmove(buf, buf + offset, bbRecv.dataLen - offset);
	}
	bbRecv.dataLen -= offset;
	assert(bbRecv.dataLen <= len);
}

void xx::UvTcpUdpBase::SendBytes(BBuffer& bb)
{
	SendBytes(bb.buf, (int)bb.dataLen);
}

void xx::UvTcpUdpBase::SendRoutingAddress(char const* const& buf, size_t const& len)
{
	assert(len <= 16);
	bbSend.Clear();
	bbSend.Reserve(3 + len);
	bbSend.buf[0] = 0;
	bbSend.buf[1] = (uint8_t)len;
	bbSend.buf[2] = (uint8_t)(len >> 8);
	memcpy(bbSend.buf + 3, buf, len);
	SendBytes(bbSend.buf, (int)(3 + len));
}

size_t xx::UvTcpUdpBase::GetRoutingAddressLength(BBuffer& bb)
{
	auto p = (uint8_t*)(bb.buf + bb.offset - 2);
	return p[0] + (p[1] << 8);
}


void xx::UvTcpUdpBase::SendRoutingByRouter(xx::BBuffer& bb, size_t const& pkgLen, size_t const& addrOffset, size_t const& addrLen, char const* const& senderAddr, size_t const& senderAddrLen)
{
	// 防止误用
	assert(bb[bb.offset] & 8);

	// 如果发送方地址长与当前包地址相同, 走捷径, 直接替换地址部分即发送. 不需要再次拼接
	if (senderAddrLen == addrLen)
	{
		memcpy(bb.buf + addrOffset, senderAddr, addrLen);
		SendBytes(bb.buf + bb.offset, (int)pkgLen);
		return;
	}

	// 否则老实拼接一次( 当然, 理论上讲调用 pair<char*, size_t>[] 的多 buf 发送版本也可以不必拼接, 这里为简化设计, 先用唯一 buf 版的函数来实现
	bbSend.Clear();
	auto newPkgLen = pkgLen - addrLen + senderAddrLen;		// 新包总长
	bbSend.Reserve(newPkgLen);

	// 用 p1 p2 定位到两个 包 的包头部位
	auto p1 = bbSend.buf;
	auto p2 = bb.buf + bb.offset;

	auto isBig = (p2[0] & 4) != 0;							// 是否大包
	size_t addr_serial_data_len = 0;						// 地址 + 流水 + 数据 的长度
	if (!isBig)
	{
		// 2字节长度
		addr_serial_data_len = (size_t)(p2[1] + (p2[2] << 8));
	}
	else
	{
		// 4字节长度
		addr_serial_data_len = (size_t)(p2[1] + (p2[2] << 8)) + (p2[3] << 16) + (p2[4] << 24);
	}
	size_t serial_data_len = addr_serial_data_len - addrLen;// 流水 + 数据 的长度

	size_t new_addr_serial_data_len = serial_data_len + senderAddrLen;	// 新包的 地址 + 流水 + 数据 的长度

																		// 如果新包加上新地址超过 2字节能表达的长度, 转为大包
	if (new_addr_serial_data_len > std::numeric_limits<uint16_t>::max())
	{
		isBig = true;
	}

	// 开始填充新包
	if (!isBig)												// 接着写长度
	{
		// 2字节长度
		p1[0] = p2[0] & 0b11111011;
		p1[1] = (uint8_t)new_addr_serial_data_len;
		p1[2] = (uint8_t)(new_addr_serial_data_len >> 8);
		memcpy(p1 + 3, senderAddr, senderAddrLen);
		memcpy(p1 + 3 + senderAddrLen, p2 + 3 + addrLen, serial_data_len);
	}
	else
	{
		// 4字节长度
		p1[0] = p2[0] | 0b00000100;
		p1[1] = (uint8_t)new_addr_serial_data_len;
		p1[2] = (uint8_t)(new_addr_serial_data_len >> 8);
		p1[3] = (uint8_t)(new_addr_serial_data_len >> 16);
		p1[4] = (uint8_t)(new_addr_serial_data_len >> 24);
		memcpy(p1 + 5, senderAddr, senderAddrLen);
		memcpy(p1 + 5 + senderAddrLen, p2 + 5 + addrLen, serial_data_len);
	}

	SendBytes(p1, (int)newPkgLen);
}

 void xx::UvTcpUdpBase::RpcTraceCallback()
{
	if (rpcSerials)
	{
		for (auto& serial : *rpcSerials)
		{
			loop.rpcMgr->Callback(serial, nullptr);
		}
		assert(rpcSerials->Empty());
	}
}



xx::UvTcpBase::UvTcpBase(UvLoop& loop)
	: UvTcpUdpBase(loop)
{
}

void xx::UvTcpBase::OnReadCBImpl(void* stream, ptrdiff_t nread, void const* buf_t)
{
	auto tcp = *((UvTcpBase**)stream - 1);
	auto mp = tcp->loop.mempool;
	auto bufPtr = ((uv_buf_t*)buf_t)->base;
	int len = (int)nread;
	if (len > 0)
	{
		auto vn = tcp->memHeader().versionNumber;
		tcp->ReceiveImpl(bufPtr, len);
		if (vn != tcp->memHeader().versionNumber)
		{
			tcp = nullptr;
		}
	}
	mp->Free(bufPtr);
	if (tcp && len < 0)
	{
		tcp->DisconnectImpl();
	}
}

void xx::UvTcpBase::SendBytes(char const* const& inBuf, int const& len)
{
	if (!addrPtr || !inBuf || !len) throw - 1;
	if (int r = TcpWrite(ptr, (char*)inBuf, len)) throw r;
}

size_t xx::UvTcpBase::GetSendQueueSize()
{
	if (!addrPtr) throw - 1;
	return uv_stream_get_write_queue_size((uv_stream_t*)ptr);
}









xx::UvTcpPeer::UvTcpPeer(UvTcpListener & listener)
	: UvTcpBase(listener.loop)
	, listener(listener)
{
	ipBuf.fill(0);

	ptr = Alloc(loop.mempool, sizeof(uv_tcp_t), this);
	if (!ptr) throw - 1;

	if (int r = uv_tcp_init((uv_loop_t*)loop.ptr, (uv_tcp_t*)ptr))
	{
		Free(loop.mempool, ptr);
		ptr = nullptr;
		throw r;
	}

	if (int r = uv_accept((uv_stream_t*)listener.ptr, (uv_stream_t*)ptr))
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw r;
	}

	if (int r = uv_read_start((uv_stream_t*)ptr, AllocCB, (uv_read_cb)OnReadCBImpl))
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw r;
	}

	addrPtr = Alloc(loop.mempool, sizeof(sockaddr_in));
	if (!addrPtr)
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw - 1;
	}

	index_at_container = listener.peers.dataLen;
	listener.peers.Add(this);
}

xx::UvTcpPeer::~UvTcpPeer()
{
	assert(addrPtr);
	RpcTraceCallback();
	if (OnDispose) OnDispose();
	Close((uv_handle_t*)ptr);
	ptr = nullptr;
	Free(loop.mempool, addrPtr);
	addrPtr = nullptr;
	listener.peers[listener.peers.dataLen - 1]->index_at_container = index_at_container;
	listener.peers.SwapRemoveAt(index_at_container);
}

void xx::UvTcpPeer::DisconnectImpl()
{
	Release();
}

bool xx::UvTcpPeer::Disconnected()
{
	return false;
}

const char* xx::UvTcpPeer::ip()
{
	if (!ptr) throw - 1;
	if (ipBuf[0]) return ipBuf.data();
	if (int r = FillIP((uv_tcp_t*)ptr, ipBuf.data(), (int)ipBuf.size())) throw r;
	return ipBuf.data();
}









xx::UvTcpClient::UvTcpClient(UvLoop& loop)
	: UvTcpBase(loop)
{
	addrPtr = Alloc(loop.mempool, sizeof(sockaddr_in));
	if (!addrPtr) throw - 1;

	index_at_container = loop.tcpClients.dataLen;
	loop.tcpClients.Add(this);
}

xx::UvTcpClient::~UvTcpClient()
{
	Disconnect();
	if (OnDispose) OnDispose();
	Free(loop.mempool, addrPtr);
	addrPtr = nullptr;
	loop.tcpClients[loop.tcpClients.dataLen - 1]->index_at_container = index_at_container;
	loop.tcpClients.SwapRemoveAt(index_at_container);
}

bool xx::UvTcpClient::alive() const
{
	return state == UvTcpStates::Connected;
}

void xx::UvTcpClient::SetAddress(char const* const& ipv4, int const& port)
{
	if (!addrPtr) throw - 1;
	if (int r = uv_ip4_addr(ipv4, port, (sockaddr_in*)addrPtr))throw r;
}

void xx::UvTcpClient::OnConnectCBImpl(void* req, int status)
{
	auto client = *((UvTcpClient**)req - 1);
	Free(client->loop.mempool, req);
	if (!client) return;
	if (status < 0)
	{
		client->Disconnect();
	}
	else
	{
		client->state = UvTcpStates::Connected;
		uv_read_start((uv_stream_t*)client->ptr, AllocCB, (uv_read_cb)OnReadCBImpl);
	}
	if (client->OnConnect) client->OnConnect(status);
}

void xx::UvTcpClient::Connect()
{
	if (!addrPtr || ptr) throw - 1;
	if (state != UvTcpStates::Disconnected) throw - 2;

	ptr = Alloc(loop.mempool, sizeof(uv_tcp_t), this);
	if (!ptr) throw - 3;

	if (int r = uv_tcp_init((uv_loop_t*)loop.ptr, (uv_tcp_t*)ptr))
	{
		Free(loop.mempool, ptr);
		ptr = nullptr;
		throw r;
	}

	auto req = (uv_connect_t*)Alloc(loop.mempool, sizeof(uv_connect_t), this);
	if (int r = uv_tcp_connect((uv_connect_t*)req, (uv_tcp_t*)ptr, (sockaddr*)addrPtr, (uv_connect_cb)OnConnectCBImpl))
	{
		Free(loop.mempool, req);
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw r;
	}

	state = UvTcpStates::Connecting;
}

int xx::UvTcpClient::TryConnect(char const* const& ipv4, int const& port)
{
	try
	{
		Disconnect();
		SetAddress(ipv4, port);
		Connect();
	}
	catch (int e)
	{
		return e;
	}
	return 0;
}

void xx::UvTcpClient::Disconnect()
{
	if (!addrPtr) return;
	if (state == UvTcpStates::Disconnected) return;
	state = UvTcpStates::Disconnected;
	RpcTraceCallback();					// 有可能再次触发 Disconnect
	if (OnDisconnect) OnDisconnect();
	Close((uv_handle_t*)ptr);
	ptr = nullptr;
	bbSend.Clear();
	bbRecv.Clear();
}

void xx::UvTcpClient::DisconnectImpl()
{
	Disconnect();
}

bool xx::UvTcpClient::Disconnected()
{
	return ptr == nullptr;
}


// todo: 简单的变野判断去掉, 没用






xx::UvTimer::UvTimer(UvLoop& loop, uint64_t const& timeoutMS, uint64_t const& repeatIntervalMS, std::function<void()>&& OnFire)
	: Object(loop.mempool)
	, OnFire(std::move(OnFire))
	, loop(loop)
{
	ptr = Alloc(loop.mempool, sizeof(uv_timer_t), this);
	if (!ptr) throw - 1;

	if (int r = uv_timer_init((uv_loop_t*)loop.ptr, (uv_timer_t*)ptr))
	{
		Free(loop.mempool, ptr);
		throw r;
	}

	if (int r = uv_timer_start((uv_timer_t*)ptr, (uv_timer_cb)OnTimerCBImpl, timeoutMS, repeatIntervalMS))
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw r;
	}

	index_at_container = loop.timers.dataLen;
	loop.timers.Add(this);
}

xx::UvTimer::~UvTimer()
{
	assert(ptr);
	Close((uv_handle_t*)ptr);
	ptr = nullptr;
	loop.timers[loop.timers.dataLen - 1]->index_at_container = index_at_container;
	loop.timers.SwapRemoveAt(index_at_container);
}

void xx::UvTimer::OnTimerCBImpl(void* handle)
{
	auto timer = *((UvTimer**)handle - 1);
	timer->OnFire();
}

void xx::UvTimer::SetRepeat(uint64_t const& repeatIntervalMS)
{
	if (!ptr) throw - 1;
	uv_timer_set_repeat((uv_timer_t*)ptr, repeatIntervalMS);
}

void xx::UvTimer::Again()
{
	if (!ptr) throw - 1;
	if (int r = uv_timer_again((uv_timer_t*)ptr)) throw r;
}

void xx::UvTimer::Stop()
{
	if (!ptr) throw - 1;
	if (int r = uv_timer_stop((uv_timer_t*)ptr)) throw r;
}









xx::UvTimeoutManager::UvTimeoutManager(UvLoop& loop, uint64_t const& intervalMS, int const& wheelLen, int const& defaultInterval)
	: Object(loop.mempool)
	, timeouterss(loop.mempool)
{
	mempool->CreateTo(timer, loop, 0, intervalMS, [this] { Process(); });
	timeouterss.Resize(wheelLen);
	this->defaultInterval = defaultInterval;
}

xx::UvTimeoutManager::~UvTimeoutManager()
{
	mempool->Release(timer); timer = nullptr;
}

void xx::UvTimeoutManager::Process()
{
	auto t = timeouterss[cursor];
	while (t)
	{
		auto nt = t->timeouterNext;
		auto vn = t->memHeader().versionNumber;
		assert(t->OnTimeout);
		t->OnTimeout();
		if (vn == t->memHeader().versionNumber)	// ensure t is never Release
		{
			t->TimeouterClear();
		}
		t = nt;
	};
	timeouterss[cursor] = nullptr;
	cursor++;
	if (cursor == timeouterss.dataLen) cursor = 0;
}

void xx::UvTimeoutManager::Clear()
{
	for (size_t i = 0; i < timeouterss.dataLen; ++i)
	{
		auto t = timeouterss[i];
		while (t)
		{
			auto nt = t->timeouterNext;
			t->TimeouterClear();
			t = nt;
		};
		timeouterss[i] = nullptr;
	}
	cursor = 0;
}

void xx::UvTimeoutManager::Add(UvTimeouterBase* const& t, int interval)
{
	if (t->timeouting()) throw - 1;
	auto timerssLen = (int)timeouterss.dataLen;
	if (!t || (interval < 0 && interval >= timerssLen)) throw - 2;
	if (interval == 0) interval = defaultInterval;

	interval += cursor;
	if (interval >= timerssLen) interval -= timerssLen;

	t->timeouterPrev = nullptr;
	t->timeouterIndex = interval;
	t->timeouterNext = timeouterss[interval];
	if (t->timeouterNext)
	{
		t->timeouterNext->timeouterPrev = t;
	}
	timeouterss[interval] = t;
}

void xx::UvTimeoutManager::Remove(UvTimeouterBase* const& t)
{
	if (!t->timeouting()) throw - 1;
	if (t->timeouterNext) t->timeouterNext->timeouterPrev = t->timeouterPrev;
	if (t->timeouterPrev) t->timeouterPrev->timeouterNext = t->timeouterNext;
	else timeouterss[t->timeouterIndex] = t->timeouterNext;
	t->TimeouterClear();
}

void xx::UvTimeoutManager::AddOrUpdate(UvTimeouterBase* const& t, int const& interval)
{
	if (t->timeouting()) Remove(t);
	Add(t, interval);
}











xx::UvAsync::UvAsync(UvLoop& loop)
	: Object(loop.mempool)
	, loop(loop)
	, actions(loop.mempool)
{
	ptr = Alloc(loop.mempool, sizeof(uv_async_t), this);
	if (!ptr) throw - 1;

	if (int r = uv_async_init((uv_loop_t*)loop.ptr, (uv_async_t*)ptr, (uv_async_cb)OnAsyncCBImpl))
	{
		Free(loop.mempool, ptr);
		ptr = nullptr;
		throw r;
	}

	OnFire = std::bind(&UvAsync::OnFireImpl, this);

	index_at_container = loop.asyncs.dataLen;
	loop.asyncs.Add(this);
}

xx::UvAsync::~UvAsync()
{
	assert(ptr);
	if (OnDispose) OnDispose();

	Close((uv_handle_t*)ptr);
	ptr = nullptr;

	loop.asyncs[loop.asyncs.dataLen - 1]->index_at_container = index_at_container;
	loop.asyncs.SwapRemoveAt(index_at_container);
}

void xx::UvAsync::OnAsyncCBImpl(void* handle)
{
	auto self = *((UvAsync**)handle - 1);
	self->OnFire();
}

void xx::UvAsync::Dispatch(std::function<void()>&& a)
{
	if (!ptr) throw - 1;
	{
		std::scoped_lock<std::mutex> g(mtx);
		actions.Push(std::move(a));
	}
	if (int r = uv_async_send((uv_async_t*)ptr)) throw r;
}

void xx::UvAsync::OnFireImpl()
{
	std::function<void()> a;
	while (true)
	{
		{
			std::scoped_lock<std::mutex> g(mtx);
			if (!actions.TryPop(a)) break;
		}
		a();
	}
}











xx::UvRpcManager::UvRpcManager(UvLoop& loop, uint64_t const& intervalMS, int const& defaultInterval)
	: Object(loop.mempool)
	, timer(nullptr)
	, mapping(loop.mempool)
	, serials(loop.mempool)
	, defaultInterval(defaultInterval)
	, serial(0)
	, ticks(0)
{
	if (defaultInterval <= 0) throw - 1;
	timer = loop.CreateTimer(0, intervalMS, [this] { Process(); });
}

xx::UvRpcManager::~UvRpcManager()
{
	if (timer)
	{
		timer->Release();
		timer = nullptr;
	}
}

void xx::UvRpcManager::Process()
{
	++ticks;
	if (serials.Empty()) return;
	while (!serials.Empty() && serials.Top().first <= ticks)
	{
		auto idx = mapping.Find(serials.Top().second);
		if (idx != -1)
		{
			auto a = std::move(mapping.ValueAt(idx));
			mapping.RemoveAt(idx);
			a(serial, nullptr);
		}
		serials.Pop();
	}
}

uint32_t xx::UvRpcManager::Register(std::function<void(uint32_t, BBuffer*)>&& cb, int interval)
{
	if (interval == 0) interval = defaultInterval;
	++serial;
	auto r = mapping.Add(serial, std::move(cb), true);
	serials.Push(std::make_pair(ticks + interval, serial));
	return serial;
}

void xx::UvRpcManager::Unregister(uint32_t const& serial)
{
	mapping.Remove(serial);
}

void xx::UvRpcManager::Callback(uint32_t const& serial, BBuffer* const& bb)
{
	int idx = mapping.Find(serial);
	if (idx == -1) return;
	auto a = std::move(mapping.ValueAt(idx));
	mapping.RemoveAt(idx);
	if (a) a(serial, bb);
}

size_t xx::UvRpcManager::Count()
{
	return serials.Count();
}










xx::UvContextBase::UvContextBase(MemPool* const& mp)
	: Object(mp)
{
}

xx::UvContextBase::~UvContextBase()
{
	KickPeer();
}

bool xx::UvContextBase::PeerAlive()
{
	return peer && !peer->Disconnected();
}

bool xx::UvContextBase::BindPeer(UvTcpUdpBase* const& p)
{
	if (peer) return false;
	p->OnReceiveRequest = std::bind(&UvContextBase::OnPeerReceiveRequest, this, std::placeholders::_1, std::placeholders::_2);
	p->OnReceivePackage = std::bind(&UvContextBase::OnPeerReceivePackage, this, std::placeholders::_1);
	p->OnDispose = std::bind(&UvContextBase::OnPeerDisconnect, this);
	peer = p;
	return true;
}

void xx::UvContextBase::KickPeer(bool const& immediately)
{
	if (peer)
	{
		peer->OnDispose = nullptr;              // 防止产生 OnDispose 调用
		peer->OnReceivePackage = nullptr;       // 清空收发包回调
		peer->OnReceiveRequest = nullptr;

		if (immediately)
		{
			peer->Release();					// 立即断开
		}
		peer = nullptr;
	}
}

void xx::UvContextBase::OnPeerReceiveRequest(uint32_t serial, BBuffer& bb)
{
	Ptr<Object> o;
	if (int r = bb.ReadRoot(o))
	{
		KickPeer();
		return;
	}
	HandleRequest(serial, o);
}

void xx::UvContextBase::OnPeerReceivePackage(BBuffer& bb)
{
	Ptr<Object> o;
	if (int r = bb.ReadRoot(o))
	{
		KickPeer();
		return;
	}
	HandlePackage(o);
}

void xx::UvContextBase::OnPeerDisconnect()
{
	KickPeer(false);
	HandleDisconnect();
}









xx::UvUdpListener::UvUdpListener(UvLoop& loop)
	: UvListenerBase(loop)
	, peers(loop.mempool)
{
	ptr = Alloc(mempool, sizeof(uv_udp_t), this);
	if (!ptr) throw - 1;

	if (int r = uv_udp_init((uv_loop_t*)loop.ptr, (uv_udp_t*)ptr))
	{
		Free(mempool, ptr);
		ptr = nullptr;
		throw - 2;
	}

	addrPtr = Alloc(mempool, sizeof(sockaddr_in));
	if (!addrPtr)
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw - 3;
	}

	index_at_container = loop.udpListeners.dataLen;
	loop.udpListeners.Add(this);
}

xx::UvUdpListener::~UvUdpListener()
{
	assert(ptr);
	if (OnDispose) OnDispose();
	for (auto& kv : peers)
	{
		kv.value->Release();
	}
	peers.Clear();
	Close((uv_handle_t*)ptr);
	ptr = nullptr;
	Free(mempool, addrPtr);
	addrPtr = nullptr;

	loop.udpListeners[loop.udpListeners.dataLen - 1]->index_at_container = index_at_container;
	loop.udpListeners.SwapRemoveAt(index_at_container);
	index_at_container = -1;
}

void xx::UvUdpListener::OnRecvCBImpl(void* uvudp, ptrdiff_t nread, void* buf_t, void* addr, uint32_t flags)
{
	auto listener = *((UvUdpListener**)uvudp - 1);
	auto mp = listener->mempool;
	auto bufPtr = ((uv_buf_t*)buf_t)->base;
	int len = (int)nread;
	if (len > 0)
	{
		listener->OnReceiveImpl(bufPtr, len, addr);
	}
	mp->Free(bufPtr);
	//if (len < 0) return;
}

void xx::UvUdpListener::OnReceiveImpl(char const* const& bufPtr, int const& len, void* const& addr)
{
	// 所有消息长度至少都有 36 字节长( Guid conv 的 kcp 头 )
	if (len < 36) return;

	// 前 16 字节转为 Guid
	Guid g(false);
	g.Fill(bufPtr);

	// 去字典中找. 没有就新建.
	int idx = peers.Find(g);
	UvUdpPeer* p = nullptr;
	if (idx < 0)
	{
		try
		{
			if (!OnCreatePeer) mempool->CreateTo(p, *this, g);
			else
			{
				p = OnCreatePeer(g);
				if (!p) return;
			}
		}
		catch (...)
		{
			return;
		}
		peers.Add(g, p);
	}
	else
	{
		p = peers.ValueAt(idx);
	}

	// 无脑更新 peer 的目标 ip 地址
	memcpy(p->addrPtr, addr, sizeof(sockaddr_in));

	if (idx < 0) OnAccept(p);

	// 转发到 peer 的 kcp
	p->Input(bufPtr, len);
}

void xx::UvUdpListener::Bind(char const* const& ipv4, int const& port)
{
	if (int r = uv_ip4_addr(ipv4, port, (sockaddr_in*)addrPtr)) throw r;
	if (int r = uv_udp_bind((uv_udp_t*)ptr, (sockaddr*)addrPtr, UV_UDP_REUSEADDR)) throw r;
}

void xx::UvUdpListener::Listen()
{
	if (int r = uv_udp_recv_start((uv_udp_t*)ptr, AllocCB, (uv_udp_recv_cb)OnRecvCBImpl)) throw r;
}

void xx::UvUdpListener::StopListen()
{
	if (int r = uv_udp_recv_stop((uv_udp_t*)ptr)) throw r;
}

xx::UvUdpPeer* xx::UvUdpListener::CreatePeer(Guid const& g
	, int const& sndwnd, int const& rcvwnd
	, int const& nodelay/*, int const& interval*/, int const& resend, int const& nc, int const& minrto)
{
	return mempool->Create<xx::UvUdpPeer>(*this, g, sndwnd, rcvwnd, nodelay/*, interval*/, resend, nc, minrto);
}








xx::UvUdpBase::UvUdpBase(UvLoop& loop)
	: UvTcpUdpBase(loop)
{
}

typedef int(*KcpOutputCB)(const char *buf, int len, ikcpcb *kcp);







xx::UvUdpPeer::UvUdpPeer(UvUdpListener& listener
	, Guid const& g
	, int const& sndwnd, int const& rcvwnd
	, int const& nodelay/*, int const& interval*/, int const& resend, int const& nc, int const& minrto)
	: UvUdpBase(listener.loop)
	, listener(listener)
{
	if (!loop.kcpInterval) throw - 1;

	ipBuf.fill(0);

	ptr = ikcp_create(&g, this, mempool);
	if (!ptr) throw - 2;

	int r = 0;
	if ((r = ikcp_wndsize((ikcpcb*)ptr, sndwnd, rcvwnd))
		|| (r = ikcp_nodelay((ikcpcb*)ptr, nodelay, loop.kcpInterval, resend, nc)))
	{
		ikcp_release((ikcpcb*)ptr);
		ptr = nullptr;
		throw r;
	}
	((ikcpcb*)ptr)->rx_minrto = minrto;
	ikcp_setoutput((ikcpcb*)ptr, (KcpOutputCB)OutputImpl);

	addrPtr = Alloc(mempool, sizeof(sockaddr_in));
	if (!addrPtr)
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw - 3;
	}

	index_at_container = listener.peers.Add(g, this).index;
}

xx::UvUdpPeer::~UvUdpPeer()
{
	RpcTraceCallback();
	if (OnDispose) OnDispose();
	Free(mempool, addrPtr);
	ikcp_release((ikcpcb*)ptr);
	ptr = nullptr;
	listener.peers.RemoveAt((int)index_at_container);
	index_at_container = -1;
}

int xx::UvUdpPeer::OutputImpl(char const* inBuf, int len, void* kcpPtr)
{
	auto peer = (UvUdpPeer*)((ikcpcb*)kcpPtr)->user;

	struct uv_udp_send_t_ex
	{
		MemPool* mp;
		uv_udp_send_t req;
		uv_buf_t buf;
	};
	auto mp = peer->mempool;
	auto req = (uv_udp_send_t_ex*)mp->Alloc(sizeof(uv_udp_send_t_ex));
	req->mp = mp;
	auto buf = (char*)mp->Alloc(len);
	memcpy(buf, inBuf, len);
	req->buf = uv_buf_init(buf, (uint32_t)len);
	return uv_udp_send((uv_udp_send_t*)req, (uv_udp_t*)peer->listener.ptr, &req->buf, 1, (sockaddr*)peer->addrPtr, [](uv_udp_send_t* req, int status)
	{
		//if (status) fprintf(stderr, "Write error: %s\n", uv_strerror(status));
		auto *wr = (uv_udp_send_t_ex*)req;
		wr->mp->Free(wr->buf.base);
		wr->mp->Free(wr);
	});
}


void xx::UvUdpPeer::Update(uint32_t const& current)
{
	if (nextUpdateTicks > current) return;
	ikcp_update((ikcpcb*)ptr, current);
	nextUpdateTicks = ikcp_check((ikcpcb*)ptr, current);

	auto versionNumber = memHeader().versionNumber;
	while (versionNumber == memHeader().versionNumber)
	{
		int len = ikcp_recv((ikcpcb*)ptr, (char*)&loop.udpRecvBuf, (int)loop.udpRecvBuf.size());
		if (len <= 0) break;
		ReceiveImpl((char*)&loop.udpRecvBuf, len);
	}
}


void xx::UvUdpPeer::Input(char const* const& data, int const& len)
{
	if (int r = ikcp_input((ikcpcb*)ptr, data, len)) throw r;
}

void xx::UvUdpPeer::SendBytes(char const* const& inBuf, int const& len)
{
	if (!addrPtr || !inBuf || !len) throw - 1;
	if (int r = ikcp_send((ikcpcb*)ptr, inBuf, len)) throw r;
}

void xx::UvUdpPeer::DisconnectImpl()
{
	Release();
}

size_t xx::UvUdpPeer::GetSendQueueSize()
{
	//return uv_udp_get_send_queue_size((uv_udp_t*)listener.ptr);
	return ikcp_waitsnd((ikcpcb*)ptr);
}

bool xx::UvUdpPeer::Disconnected()
{
	return false;
}

char* xx::UvUdpPeer::ip()
{
	if (!ptr) throw - 1;
	if (ipBuf[0]) return ipBuf.data();
	if (int r = FillIP((sockaddr_in*)addrPtr, ipBuf.data(), ipBuf.size())) throw r;
	return ipBuf.data();
}









xx::UvUdpClient::UvUdpClient(UvLoop& loop)
	: UvUdpBase(loop)
{
	addrPtr = Alloc(mempool, sizeof(sockaddr_in));
	if (!addrPtr)
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw - 1;
	}

	index_at_container = loop.udpClients.dataLen;
	loop.udpClients.Add(this);

}
xx::UvUdpClient::~UvUdpClient()
{
	RpcTraceCallback();
	if (OnDispose) OnDispose();

	Disconnect();

	loop.udpClients.RemoveAt(index_at_container);
	index_at_container = (size_t)-1;
}

void xx::UvUdpClient::Connect(xx::Guid const& g
	, int const& sndwnd, int const& rcvwnd
	, int const& nodelay/*, int const& interval*/, int const& resend, int const& nc, int const& minrto)
{
	if (!loop.kcpInterval) throw - 1;

	if (ptr) throw - 2;

	ptr = Alloc(mempool, sizeof(uv_udp_t), this);
	if (!ptr) throw - 3;

	int r = 0;
	if ((r = uv_udp_init((uv_loop_t*)loop.ptr, (uv_udp_t*)ptr)))
	{
		Free(mempool, ptr);
		ptr = nullptr;
		throw r;
	}

	if ((r = uv_udp_recv_start((uv_udp_t*)ptr, AllocCB, (uv_udp_recv_cb)OnRecvCBImpl)))
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw r;
	}

	this->guid = g;
	kcpPtr = ikcp_create(&g, this, mempool);
	if (!kcpPtr)
	{
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw - 4;
	}

	if ((r = ikcp_wndsize((ikcpcb*)kcpPtr, sndwnd, rcvwnd))
		|| (r = ikcp_nodelay((ikcpcb*)kcpPtr, nodelay, loop.kcpInterval, resend, nc)))
	{
		ikcp_release((ikcpcb*)kcpPtr);
		kcpPtr = nullptr;
		Close((uv_handle_t*)ptr);
		ptr = nullptr;
		throw - 5;
	}
	((ikcpcb*)ptr)->rx_minrto = minrto;
	ikcp_setoutput((ikcpcb*)kcpPtr, (KcpOutputCB)OutputImpl);
}
void xx::UvUdpClient::OnRecvCBImpl(void* uvudp, ptrdiff_t nread, void* buf_t, void* addr, uint32_t flags)
{
	auto client = *((UvUdpClient**)uvudp - 1);
	auto mp = client->mempool;
	auto bufPtr = ((uv_buf_t*)buf_t)->base;
	int len = (int)nread;
	if (len > 0)
	{
		client->OnReceiveImpl(bufPtr, len, addr);
	}
	mp->Free(bufPtr);
	//if (len < 0) return;
}
void xx::UvUdpClient::OnReceiveImpl(char const* const& bufPtr, int const& len, void* const& addr)
{
	if (len < 36) return;
	if (int r = ikcp_input((ikcpcb*)kcpPtr, bufPtr, len)) throw r;
}
int xx::UvUdpClient::OutputImpl(char const* inBuf, int len, void* kcpPtr)
{
	auto client = (UvUdpClient*)((ikcpcb*)kcpPtr)->user;

	struct uv_udp_send_t_ex
	{
		MemPool* mp;
		uv_udp_send_t req;
		uv_buf_t buf;
	};
	auto mp = client->mempool;
	auto req = (uv_udp_send_t_ex*)mp->Alloc(sizeof(uv_udp_send_t_ex));
	req->mp = mp;
	auto buf = (char*)mp->Alloc(len);
	memcpy(buf, inBuf, len);
	req->buf = uv_buf_init(buf, (uint32_t)len);
	return uv_udp_send((uv_udp_send_t*)req, (uv_udp_t*)client->ptr, &req->buf, 1, (sockaddr*)client->addrPtr, [](uv_udp_send_t* req, int status)
	{
		//if (status) fprintf(stderr, "Write error: %s\n", uv_strerror(status));
		auto *wr = (uv_udp_send_t_ex*)req;
		wr->mp->Free(wr->buf.base);
		wr->mp->Free(wr);
	});
}
void xx::UvUdpClient::Update(uint32_t const& current)
{
	if (nextUpdateTicks > current) return;
	ikcp_update((ikcpcb*)kcpPtr, current);
	nextUpdateTicks = ikcp_check((ikcpcb*)kcpPtr, current);

	auto versionNumber = memHeader().versionNumber;
	while (versionNumber == memHeader().versionNumber)
	{
		int len = ikcp_recv((ikcpcb*)kcpPtr, (char*)&loop.udpRecvBuf, (int)loop.udpRecvBuf.size());
		if (len <= 0) break;
		ReceiveImpl((char*)&loop.udpRecvBuf, len);
	}
}
void xx::UvUdpClient::Disconnect()
{
	if (!ptr) return;

	Close((uv_handle_t*)ptr);
	ptr = nullptr;

	ikcp_release((ikcpcb*)kcpPtr);
	kcpPtr = nullptr;
}
void xx::UvUdpClient::SetAddress(char const* const& ipv4, int const& port)
{
	if (int r = uv_ip4_addr(ipv4, port, (sockaddr_in*)addrPtr)) throw r;
}
void xx::UvUdpClient::SendBytes(char const* const& inBuf, int const& len)
{
	if (!addrPtr || !inBuf || !len) throw - 1;
	if (int r = ikcp_send((ikcpcb*)kcpPtr, inBuf, len)) throw r;
}
void xx::UvUdpClient::DisconnectImpl()
{
	Disconnect();
}
bool xx::UvUdpClient::Disconnected()
{
	return kcpPtr != nullptr;
}
size_t xx::UvUdpClient::GetSendQueueSize()
{
	//return uv_udp_get_send_queue_size((uv_udp_t*)ptr);
	if (!kcpPtr) return 0;
	return ikcp_waitsnd((ikcpcb*)kcpPtr);
}

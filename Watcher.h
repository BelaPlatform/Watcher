#pragma once

#include <vector>
#include <typeinfo>
#include <string>
#include <new> // for std::bad_alloc
#include <unistd.h>
#include <atomic>
#include <libraries/Pipe/Pipe.h>
#include <string.h>

class WatcherManager;
class WatcherBase {
public:
	void localControl(bool enable) {
		if(localControlEnabled != enable)
		{
			localControlEnabled = enable;
			localControlChanged();
		}
	}
	virtual double wmGet() = 0;
	virtual double wmGetInput() = 0;
	virtual void wmSet(double) = 0;
	virtual void wmSetMask(unsigned int, unsigned int) = 0;
	virtual void localControlChanged() {}
	bool hasLocalControl() {
		return localControlEnabled;
	}
protected:
	bool localControlEnabled = true;
};

#include <algorithm>
#include <vector>
#include <libraries/Gui/Gui.h>
#include <libraries/WriteFile/WriteFile.h>

#include <thread>
class WatcherManager
{
	static constexpr uint32_t kMonitorDont = 0;
	static constexpr uint32_t kMonitorChange = 1 << 31;
	typedef uint64_t AbsTimestamp;
	typedef uint32_t RelTimestamp;
	struct Priv;
	std::thread pipeToJsonThread;
	AbsTimestamp timestamp = 0;
	size_t pipeReceivedRt = 0;
	size_t pipeSentNonRt = 0;
	Pipe pipe;
	volatile bool shouldStop;
	static constexpr size_t kMsgHeaderLength = sizeof(timestamp);
	static_assert(0 == kMsgHeaderLength % sizeof(float), "has to be multiple");
	static constexpr size_t kBufSize = 4096 + kMsgHeaderLength;
	static size_t getRelTimestampsOffset(size_t dataSize);
public:
	WatcherManager(Gui& gui);
	~WatcherManager();
	void setup(float sampleRate);
	class Details;
	enum TimestampMode {
		kTimestampBlock,
		kTimestampSample,
	};
	template <typename T>
	Details* reg(WatcherBase* that, const std::string& name, TimestampMode timestampMode)
	{
		return doReg(that, name, timestampMode, typeid(T).name(), sizeof(T));
	}
	void unreg(WatcherBase* that);
	void tick(AbsTimestamp frames, bool full = true)
	{
		timestamp = frames;
		if(!full)
			return;
		while(pipeReceivedRt != pipeSentNonRt)
		{
			MsgToRt msg;
			if(1 == pipe.readRt(msg))
			{
				pipeReceivedRt++;
				switch(msg.cmd)
				{
					case MsgToRt::kCmdStartLogging:
						startLogging(msg.priv, msg.args[0], msg.args[1]);
						break;
					case MsgToRt::kCmdStopLogging:
						stopLogging(msg.priv, msg.args[0]);
						break;
					case MsgToRt::kCmdStartWatching:
						startWatching(msg.priv, msg.args[0], msg.args[1]);
						break;
					case MsgToRt::kCmdStopWatching:
						stopWatching(msg.priv, msg.args[0]);
						break;
					case MsgToRt::kCmdNone:
						break;
				}
			} else {
				rt_fprintf(stderr, "Error: missing messages in the pipe\n");
				pipeReceivedRt = pipeSentNonRt;
			}
		}
		clientActive = gui.numActiveConnections();
	}
	void updateSometingToDo(Priv* p, bool should = false)
	{
		for(auto& stream : p->streams)
		{
			should |= (stream.schedTsStart != -1);
			should |= stream.state;
		}
		should |= (kMonitorDont != p->monitoring);
		// TODO: is watching should be conditional to && clientActive,
		// but for that to work, we'd need to call this for each client
		// on clientActive change, which could be very expensive
		should |= (isStreaming(p, kStreamIdxWatch)) || isStreaming(p, kStreamIdxLog);
		p->somethingToDo = should;
	}
	// the relevant object is passed back here so that we don't have to waste
	// time looking it up
	template <typename T>
	void notify(Details* d, const T& value)
	{
		Priv* p = reinterpret_cast<Priv*>(d);
		if(!p)
			return;
		if(!p->somethingToDo)
			return;
		bool streamLast = false;
		for(auto& stream : p->streams)
		{
			if(timestamp >= stream.schedTsStart)
			{
				stream.schedTsStart = -1;
				if(kStreamStateStarting == stream.state)
				{
					stream.state = kStreamStateYes;
					// TODO: watching and logging use the same buffer,
					// so you'll get a dropout in the watching if you
					// are watching right now
					p->count = 0;
					if(-1 != stream.schedTsEnd) {
						// if an end timestamp is provided,
						// schedule the end immediately
						stream.schedTsStart = stream.schedTsEnd;
						stream.state = kStreamStateStopping;
					}
				}
				else if(kStreamStateStopping == stream.state)
				{
					stream.state = kStreamStateLast;
					streamLast = true;
				}
				updateSometingToDo(p);
			}
		}
		if(kMonitorDont != p->monitoring)
		{
			if(p->monitoring & kMonitorChange)
			{
				p->monitoring &= ~kMonitorChange; // reset flag
				if(p->monitoring) {
					// trigger to send one immediately
					p->monitoringNext = timestamp;
				} else
					p->monitoringNext = -1;
			}
			if(timestamp >= p->monitoringNext)
			{
				if(clientActive)
				{
					// big enough for the timestamp and one value
					// and possibly some padding bytes at the end
					// (though in practice there won't be any when
					// sizeof(T) <= kMsgHeaderLength)
					uint8_t data[((kMsgHeaderLength + sizeof(value) + sizeof(T) - 1) / sizeof(T)) * sizeof(T)];
					memcpy(data, &timestamp, kMsgHeaderLength);
					memcpy(data + kMsgHeaderLength, &value, sizeof(value));
					gui.sendBuffer(p->guiBufferId, (T*)data, sizeof(data) / sizeof(T));
				}
				if(1 == p->monitoring)
				{
					// special case: one-shot
					// so disable at the next iteration
					p->monitoring = kMonitorChange | 0;
					updateSometingToDo(p);
				} else
					p->monitoringNext = timestamp + p->monitoring;
			}
		}
		if((isStreaming(p, kStreamIdxWatch) && clientActive) || isStreaming(p, kStreamIdxLog))
		{
			if(0 == p->count)
			{
				memcpy(p->v.data(), &timestamp, kMsgHeaderLength);
				p->firstTimestamp = timestamp;
				p->count += kMsgHeaderLength;
				p->countRelTimestamps = p->relTimestampsOffset;
			}
			*(T*)(p->v.data() + p->count) = value;
			p->count += sizeof(value);
			bool full = p->count >= p->maxCount;
			if(kTimestampSample == p->timestampMode)
			{
				// we have two arrays: one of type T starting
				// at kMsgHeaderLength and one of type
				// RelTimestamp starting at relTimestampsOffset
				RelTimestamp relTimestamp = timestamp - p->firstTimestamp;
				*(RelTimestamp*)(p->v.data() + p->countRelTimestamps) = relTimestamp;
				p->countRelTimestamps += sizeof(relTimestamp);
				full |= (p->count >= p->relTimestampsOffset || p->countRelTimestamps >= p->v.size());
			} else {
				// only one array of type T starting at
				// kMsgHeaderLength
			}
			if(full || streamLast)
			{
				if(!full)
				{
					// when logging stops, we need to fill
					// up all the remaining space with zeros
					// TODO: remove this when we support
					// variable-length blocks
					if(kTimestampSample == p->timestampMode)
					{
						memset(p->v.data() + p->count, 0, p->relTimestampsOffset - p->count);
						memset(p->v.data() + p->countRelTimestamps, 0, p->v.size() - p->countRelTimestamps);
					} else
						memset(p->v.data() + p->count, 0, p->v.size() - p->count);
				}
				// TODO: in order to even out the CPU load,
				// incoming data should be copied out of the
				// audio thread one value at a time
				// avoiding big copies like this one
				// OTOH, we'll need to ensure only full blocks
				// are sent so that we don't lose track of the
				// header

				send<T>(p);
				bool shouldUpdate = false;
				for(size_t n = 0; n < p->streams.size(); ++n)
				{
					Stream& stream = p->streams[n];
					if(kStreamStateLast == stream.state)
					{
						if(kStreamIdxLog == n)
							p->logger->requestFlush();
						stream.state = kStreamStateNo;
						shouldUpdate = true;
					}
				}
				if(shouldUpdate)
					updateSometingToDo(p);
				p->count = 0;
			}
		}
	}
	Gui& getGui() {
		return gui;
	}
private:
	enum StreamIdx {
		kStreamIdxLog,
		kStreamIdxWatch,
		kStreamIdxNum,
	};
	enum StreamState {
		kStreamStateNo,
		kStreamStateStarting,
		kStreamStateYes,
		kStreamStateStopping,
		kStreamStateLast,
	};
	struct Stream {
		AbsTimestamp schedTsStart = -1;
		AbsTimestamp schedTsEnd = -1;
		StreamState state = kStreamStateNo;
	};
	struct Priv {
		WatcherBase* w;
		std::vector<unsigned char> v;
		size_t count;
		std::string name;
		unsigned int guiBufferId;
		WriteFile* logger;
		std::string logFileName;
		std::string type;
		TimestampMode timestampMode;
		AbsTimestamp firstTimestamp;
		size_t relTimestampsOffset;
		size_t countRelTimestamps;
		size_t maxCount;
		uint32_t monitoring;
		AbsTimestamp monitoringNext;
		std::array<Stream,kStreamIdxNum> streams;
		bool controlled;
		bool somethingToDo;
	};
	struct MsgToNrt {
		Priv* priv;
		enum Cmd {
			kCmdNone,
			kCmdStartedLogging,
		} cmd;
		uint64_t args[2];
	};
	struct MsgToRt {
		Priv* priv;
		enum Cmd {
			kCmdNone,
			kCmdStartLogging,
			kCmdStopLogging,
			kCmdStartWatching,
			kCmdStopWatching,
		} cmd;
		uint64_t args[2];
	};
	void pipeToJson();
	bool isStreaming(const Priv* p, StreamIdx idx) const
	{
		StreamState state = p->streams[idx].state;
		return kStreamStateYes == state || kStreamStateStopping == state|| kStreamStateLast == state;
	}
	template <typename T>
	void send(Priv* p) {
		size_t size = p->v.size(); // TODO: customise this for smaller frames
		if(clientActive && isStreaming(p, kStreamIdxWatch))
			gui.sendBuffer(p->guiBufferId, (T*)p->v.data(), size / sizeof(T));
		if(isStreaming(p, kStreamIdxLog))
			p->logger->log((float*)p->v.data(), size / sizeof(float));
	}
	void startWatching(Priv* p, AbsTimestamp startTimestamp, AbsTimestamp duration);
	void stopWatching(Priv* p, AbsTimestamp timestampEnd);
	void startControlling(Priv* p);
	void stopControlling(Priv* p);
	void startStreamAtFor(Priv* p, StreamIdx idx, AbsTimestamp startTimestamp, AbsTimestamp duration);
	void stopStreamAt(Priv* p, StreamIdx idx, AbsTimestamp timestampEnd);
	void startLogging(Priv* p, AbsTimestamp startTimestamp, AbsTimestamp duration);
	void stopLogging(Priv* p, AbsTimestamp timestamp);
	void setMonitoring(Priv* p, size_t period);
	void setupLogger(Priv* p);
	void cleanupLogger(Priv* p);
	Priv* findPrivByName(const std::string& str);
	void sendJsonResponse(JSONValue* watcher, WSServer::CallingThread thread);
	bool controlCallback(JSONObject& root);
	Details* doReg(WatcherBase* that, std::string name, TimestampMode timestampMode, const std::string& typeName, size_t typeSize);
	std::vector<Priv*> vec;
	float sampleRate = 0;
	Gui& gui;
	bool clientActive = true;
};

#ifndef WATCHER_DISABLE_DEFAULT
WatcherManager* Bela_getDefaultWatcherManager();
#endif // WATCHER_DISABLE_DEFAULT

template <typename T>
class Watcher : public WatcherBase {
    static_assert(
	std::is_same<T,char>::value
	|| std::is_same<T,unsigned int>::value
	|| std::is_same<T,int>::value
	|| std::is_same<T,float>::value
	|| std::is_same<T,double>::value
	, "T is not of a supported type");
public:
	Watcher() = default;
	Watcher(WatcherManager& wm, T value = 0) : Watcher("", WatcherManager::kTimestampBlock, &wm, value) {}
	Watcher(const std::string& name, WatcherManager& wm, T value = 0) : Watcher(name, WatcherManager::kTimestampBlock, &wm, value) {}
#ifndef WATCHER_DISABLE_DEFAULT
	Watcher(T value = 0) : Watcher("", WatcherManager::kTimestampBlock, Bela_getDefaultWatcherManager(), value) {}
	Watcher(const std::string& name, T value = 0) : Watcher(name, WatcherManager::kTimestampBlock, Bela_getDefaultWatcherManager(), value) {}
#endif // ! WATCHER_DISABLE_DEFAULT
#ifdef WATCHER_DISABLE_DEFAULT
	Watcher(const std::string& name, WatcherManager::TimestampMode timestampMode, WatcherManager* wm, T value = 0)
#else
	Watcher(const std::string& name, WatcherManager::TimestampMode timestampMode = WatcherManager::kTimestampBlock, WatcherManager* wm = Bela_getDefaultWatcherManager(), T value = 0)
#endif
		:
		wm(wm)
	{
		if(wm)
			d = wm->reg<T>(this, name, timestampMode);
		set(value);
	}
	virtual ~Watcher() {
		if(wm)
			wm->unreg(this);
	}
	operator T()
	{
		return get();
	}
	double wmGet() override
	{
		return get();
	}
	double wmGetInput() override {
		return v;
	}
	void wmSet(double value) override
	{
		vr = value;
	}
	// TODO: figure out how to provide  NOP alternative via enable_if for
	// non-integer types
	// template <typename = std::enable_if<std::is_integral<T>::value>>
	void wmSetMask(unsigned int value, unsigned int mask) override
	{
		this->mask = mask;
		vr = ((unsigned int)vr & ~mask) | (value & mask);
	}
	unsigned int getMask()
	{
		return this->mask;
	}
	// TODO: use template functions to cast to numerical types if T is numerical
	void operator=(T value) {
		set(value);
	}
	void set(const T& value) {
		v = value;
		if(wm)
			wm->notify(d, v);
	}
	void localControlChanged() override
	{
		// if disabling local control, initialise the remote value with
		// the current value
		if(!localControlEnabled)
			vr = v;
	}
	T get() {
		if(localControlEnabled)
			return v;
		else
			return vr;
	}
protected:
	T v {};
	T vr {};
	WatcherManager* wm;
	WatcherManager::Details* d;
	unsigned int mask;
};

#include <Bela.h>
#include "Watcher.h"

static inline const JSONArray& JSONGetArray(JSONObject& root, const std::string& key)
{
	static const JSONArray empty;
	std::wstring ws = JSON::s2ws(key);
	if(root.find(ws) == root.end())
		return empty;
	if(!root[ws]->IsArray())
		return empty;
	return root[ws]->AsArray();
}

static inline const JSONArray& JSONGetArray(JSONValue* el, const std::string& key)
{
	static const JSONArray empty;
	std::wstring ws = JSON::s2ws(key);
	const wchar_t* wkey = ws.c_str();
	if(el->HasChild(wkey) && el->Child(wkey)->IsArray())
		return el->Child(wkey)->AsArray();
	else
		return empty;
}

static inline std::string JSONGetAsString(JSONValue* el)
{
	if(el->IsString())
		return JSON::ws2s(el->AsString());
	else
		return "";
}

static inline std::string JSONGetString(JSONValue* el, const std::string& key)
{
	std::wstring ws = JSON::s2ws(key);
	const wchar_t* wkey = ws.c_str();
	if(el->HasChild(wkey))
		return JSONGetAsString(el->Child(wkey));
	else
		return "";
}

static inline double JSONGetAsNumber(JSONValue* el)
{
	if(el->IsNumber())
		return el->AsNumber();
	else if(el->IsBool())
		return el->AsBool();
	else
		return 0;
}
template <typename T>
static inline double _JSONGetNumber(JSONValue* el, const T& key)
{
	if(el->HasChild(key))
	{
		JSONValue* child = el->Child(key);
		return JSONGetAsNumber(child);
	}
	return 0;
}
static inline double JSONGetNumber(JSONValue* el, size_t key)
{
	return _JSONGetNumber(el, key);
}

// the below is deprecated because it's probably broken:
// JSONValue will destroy arr (or its contents?) not sure, so test it  before use
static inline double JSONGetNumber(const JSONArray& arr, size_t key) __attribute__ ((deprecated("Not actually deprecated, but probably broken. Needs testing")));
static inline double JSONGetNumber(const JSONArray& arr, size_t key)
{
	JSONValue value(arr);
	return _JSONGetNumber(&value, key);
}

static inline double JSONGetNumber(JSONValue* el, const std::string& key)
{
	std::wstring ws = JSON::s2ws(key);
	const wchar_t* wkey = ws.c_str();
	return _JSONGetNumber(el, wkey);
}

WatcherManager* Bela_getDefaultWatcherManager()
{
	static Gui gui;
	static WatcherManager defaultWatcherManager(gui);
	return &defaultWatcherManager;
}


	size_t WatcherManager::getRelTimestampsOffset(size_t dataSize)
	{
		size_t maxElements = (kBufSize - kMsgHeaderLength) / (dataSize + sizeof(RelTimestamp));
		size_t offset = maxElements * dataSize + kMsgHeaderLength;
		// round down to nearest aligned byte
		offset = offset & ~(sizeof(RelTimestamp) - 1);
		return offset;
	}
	WatcherManager::WatcherManager(Gui& gui) : pipe(std::string("watcherManager") + std::to_string((unsigned)this), 65536, true, true), gui(gui)
	{
		gui.setControlDataCallback([this](JSONObject& json, void*) {
			this->controlCallback(json);
			return true;
		});
		pipe.setTimeoutMsNonRt(100);
		shouldStop = false;
		pipeToJsonThread = std::thread(&WatcherManager::pipeToJson, this);
	};
	WatcherManager::~WatcherManager()
	{
		shouldStop = true;
		pipeToJsonThread.join();
	}
	void WatcherManager::setup(float sampleRate)
	{
		this->sampleRate = sampleRate;
	}
	void WatcherManager::unreg(WatcherBase* that)
	{
		auto it = std::find_if(vec.begin(), vec.end(), [that](decltype(vec[0])& item){
			return item->w == that;
		});
		cleanupLogger(*it);
		// TODO: unregister from GUI
		delete *it;
		vec.erase(it);
	}
	void WatcherManager::pipeToJson()
	{
		while(!shouldStop)
		{
			struct MsgToNrt msg;
			if(1 == pipe.readNonRt(msg))
			{
				switch(msg.cmd)
				{
					case MsgToNrt::kCmdStartedLogging:
					{
						JSONObject watcher;
						watcher[L"watcher"] = new JSONValue(JSON::s2ws(msg.priv->name));
						watcher[L"logFileName"] = new JSONValue(JSON::s2ws(msg.priv->logFileName));
						watcher[L"timestamp"] = new JSONValue(double(msg.args[0]));
						watcher[L"timestampEnd"] = new JSONValue(double(msg.args[1]));
						sendJsonResponse(new JSONValue(watcher), WSServer::kThreadOther);
					}
						break;
					case MsgToNrt::kCmdNone:
						break;
				}
			}
		}
	}
	void WatcherManager::startWatching(Priv* p, AbsTimestamp startTimestamp, AbsTimestamp duration) {
		startStreamAtFor(p, kStreamIdxWatch, startTimestamp, duration);
		// TODO: register guiBufferId here
	}
	void WatcherManager::stopWatching(Priv* p, AbsTimestamp timestampEnd) {
		stopStreamAt(p, kStreamIdxWatch, timestampEnd);
		// TODO: unregister guiBufferId here
	}
	void WatcherManager::startControlling(Priv* p) {
		if(p->controlled)
			return;
		p->controlled = true;
		p->w->localControl(false);
	}
	void WatcherManager::stopControlling(Priv* p) {
		if(!p->controlled)
			return;
		p->controlled = false;
		p->w->localControl(true);
	}
	void WatcherManager::startStreamAtFor(Priv* p, StreamIdx idx, AbsTimestamp startTimestamp, AbsTimestamp duration) {
		Stream& stream = p->streams[idx];
		stream.state = kStreamStateStarting;
		if(startTimestamp < timestamp)
			startTimestamp = timestamp;
		stream.schedTsStart = startTimestamp;
		AbsTimestamp timestampEnd = startTimestamp + duration;
		if(0 == duration)
			timestampEnd = -1; // do not stop automatically
		stream.schedTsEnd = timestampEnd;
		if(kStreamIdxLog == idx) {
			// send a response with the actual timestamps
			MsgToNrt msg {
				.priv = p,
				.cmd = MsgToNrt::kCmdStartedLogging,
				.args = {
					startTimestamp,
					timestampEnd,
				},
			};
			pipe.writeRt(msg);
		}
		updateSometingToDo(p, true);
	}
	void WatcherManager::stopStreamAt(Priv* p, StreamIdx idx, AbsTimestamp timestampEnd) {
		Stream& stream = p->streams[idx];
		if(kStreamStateNo == stream.state)
			return;
		stream.state = kStreamStateStopping;
		stream.schedTsStart = timestampEnd;
		updateSometingToDo(p, true);
	}
	void WatcherManager::startLogging(Priv* p, AbsTimestamp startTimestamp, AbsTimestamp duration) {
		startStreamAtFor(p, kStreamIdxLog, startTimestamp, duration);
	}
	void WatcherManager::stopLogging(Priv* p, AbsTimestamp timestamp) {
		stopStreamAt(p, kStreamIdxLog, timestamp);
	}
	void WatcherManager::setMonitoring(Priv* p, size_t period) {
		p->monitoring = (kMonitorChange | period);
		p->somethingToDo = true; // TODO: race condition
	}
	void WatcherManager::setupLogger(Priv* p) {
		cleanupLogger(p);
		p->logger = new WriteFile((p->name + ".bin").c_str(), false, false);
		p->logger->setFileType(kBinary);
		p->logFileName = p->logger->getName();
		std::vector<uint8_t> header;
		// string fields first, null-separated
		for(auto c : std::string("watcher"))
			header.push_back(c);
		header.push_back(0);
		for(auto c : p->name)
			header.push_back(c);
		header.push_back(0);
		for(auto c : p->type)
			header.push_back(c);
		header.push_back(0);
		pid_t pid = getpid();
		for(size_t n = 0; n < sizeof(pid); ++n)
			header.push_back(((uint8_t*)&pid)[n]);
		decltype(this) ptr = this;
		for(size_t n = 0; n < sizeof(ptr); ++n)
			header.push_back(((uint8_t*)&ptr)[n]);
		header.resize(((header.size() + 3) / 4) * 4); // round to nearest multiple of 4
		p->logger->log((float*)(header.data()), header.size() / sizeof(float));
	}

	void WatcherManager::cleanupLogger(Priv* p) {
		if(!p || !p->logger)
			return;
		p->logger->cleanup(false);
		delete p->logger;
		p->logger = nullptr;
	}

	WatcherManager::Priv* WatcherManager::findPrivByName(const std::string& str) {
		auto it = std::find_if(vec.begin(), vec.end(), [&str](decltype(vec[0])& item) {
			return item->name == str;
		});
		if(it != vec.end())
			return *it;
		else
			return nullptr;
	}
	void WatcherManager::sendJsonResponse(JSONValue* watcher, WSServer::CallingThread thread)
	{
		// should be called from the controlCallback() thread
		JSONObject root;
		root[L"watcher"] = watcher;
		JSONValue value(root);
		gui.sendControl(&value, thread);
	}
	bool WatcherManager::controlCallback(JSONObject& root)
	{
		auto watcher = JSONGetArray(root, "watcher");
		for(size_t n = 0; n < watcher.size(); ++n)
		{
			JSONValue* el = watcher[n];
			std::string cmd = JSONGetString(el, "cmd");
			if("list" == cmd)
			{
				// send watcher list JSON
				JSONArray watchers;
				for(auto& item : vec)
				{
					auto& v = *item;
					JSONObject watcher;
					watcher[L"name"] = new JSONValue(JSON::s2ws(v.name));
					watcher[L"watched"] = new JSONValue(isStreaming(&v, kStreamIdxWatch));
					watcher[L"controlled"] = new JSONValue(v.controlled);
					watcher[L"logged"] = new JSONValue(isStreaming(&v, kStreamIdxLog));
					watcher[L"monitor"] = new JSONValue(int((~kMonitorChange) & v.monitoring));
					watcher[L"logFileName"] = new JSONValue(JSON::s2ws(v.logFileName));
					watcher[L"value"] = new JSONValue(v.w->wmGet());
					watcher[L"valueInput"] = new JSONValue(v.w->wmGetInput());
					watcher[L"type"] = new JSONValue(JSON::s2ws(v.type));
					watcher[L"timestampMode"] = new JSONValue(v.timestampMode);
					watchers.emplace_back(new JSONValue(watcher));
				}
				JSONObject watcher;
				watcher[L"watchers"] = new JSONValue(watchers);
				watcher[L"sampleRate"] = new JSONValue(float(sampleRate));
				watcher[L"timestamp"] = new JSONValue(double(timestamp));
				sendJsonResponse(new JSONValue(watcher), WSServer::kThreadCallback);
			} else
			if("watch" == cmd || "unwatch" == cmd || "control" == cmd || "uncontrol" == cmd || "log" == cmd || "unlog" == cmd || "monitor" == cmd) {
				const JSONArray& watchers = JSONGetArray(el, "watchers");
				const JSONArray& periods = JSONGetArray(el, "periods"); // used only by 'monitor'
				const JSONArray& timestamps = JSONGetArray(el, "timestamps"); // used only by some commands
				const JSONArray& durations = JSONGetArray(el, "durations"); // used only by some commands
				size_t numSent = 0;
				for(size_t n = 0; n < watchers.size(); ++n)
				{
					std::string str = JSONGetAsString(watchers[n]);
					Priv* p = findPrivByName(str);
#ifdef WATCHER_PRINT
					printf("%s {'%s', %p}, ", cmd.c_str(), str.c_str(), p);
#endif // WATCHER_PRINT
					if(p)
					{
						AbsTimestamp timestamp = 0;
						AbsTimestamp duration = 0;
						MsgToRt msg {
							.priv = p,
							.cmd = MsgToRt::kCmdNone,
						};
						if(n < timestamps.size())
							timestamp = JSONGetAsNumber(timestamps[n]);
						if(n < durations.size())
							duration = JSONGetAsNumber(durations[n]);
						if("watch" == cmd) {
							msg.cmd = MsgToRt::kCmdStartWatching;
							msg.args[0] = timestamp;
							msg.args[1] = duration;
						} else if("unwatch" == cmd) {
							msg.cmd = MsgToRt::kCmdStopWatching;
							msg.args[0] = timestamp;
						}
						else if("control" == cmd)
							startControlling(p);
						else if("uncontrol" == cmd)
							stopControlling(p);
						else if("log" == cmd) {
							if(isStreaming(p, kStreamIdxLog))
								continue;
							msg.cmd = MsgToRt::kCmdStartLogging;
							msg.args[0] = timestamp;
							msg.args[1] = duration;
							setupLogger(p);
						} else if("unlog" == cmd) {
							msg.cmd = MsgToRt::kCmdStopLogging;
							msg.args[0] = timestamp;
						} else if ("monitor" == cmd) {
							if(n < periods.size())
							{
								size_t period = JSONGetAsNumber(periods[n]);
								setMonitoring(p, period);
							} else {
								fprintf(stderr, "monitor cmd with not enough elements in periods: %u instead of %u\n", periods.size(), watchers.size());
								break;
							}
						}
						if(MsgToRt::kCmdNone != msg.cmd)
						{
							pipe.writeNonRt(msg);
							numSent++;
						}
					}
				}
#ifdef WATCHER_PRINT
				printf("\n");
#endif // WATCHER_PRINT
				if(numSent)
				{
					// this full memory barrier may be
					// unnecessary as the system calls in Pipe::writeRt()
					// may be enough
					// or it may be useless and still leave the problem unaddressed
					std::atomic_thread_fence(std::memory_order_release);
					pipeSentNonRt += numSent;
				}
			} else
			if("set" == cmd || "setMask" == cmd) {
				const JSONArray& watchers = JSONGetArray(el, "watchers");
				const JSONArray& values = JSONGetArray(el, "values");
				const JSONArray& masks = JSONGetArray(el, "masks");
				if(watchers.size() != values.size()) {
					fprintf(stderr, "set: incompatible size of watchers and values\n");
					return false;
				}
				for(size_t n = 0; n < watchers.size(); ++n)
				{
					std::string name = JSONGetAsString(watchers[n]);
					double val = JSONGetAsNumber(values[n]);
					Priv* p = findPrivByName(name);
					if(p)
					{
						if("set" == cmd)
							p->w->wmSet(val);
						else if("setMask" == cmd) {
							if(n > masks.size())
								break;
							unsigned int mask = JSONGetAsNumber(masks[n]);
							p->w->wmSetMask(val, mask);
						}
					}
				}
			} else
				printf("Unhandled command cmd: %s\n", cmd.c_str());
		}
		return false;
	}
	WatcherManager::Details* WatcherManager::doReg(WatcherBase* that, std::string name, TimestampMode timestampMode, const std::string& typeName, size_t typeSize)
	{
		if("" == name)
			name = "(anon)";
		// sanitise
		constexpr char kReserved = '~';
		for(auto& c : name)
		{
			if(kReserved == c)
				c = '_';
		}
		if(findPrivByName(name))
		{
			// name already exists, append number
			name += kReserved;
			unsigned int count = 1; // if it's the first instance, start from 1
			for(ssize_t n = vec.size() - 1; n >= 0; --n)
			{
				std::string other = vec[n]->name;
				if(other.size() <= name.size())
					continue;
				if(other.substr(0, name.size()) == other)
				{
					count = std::stoi(other.substr(name.size() + 1));
					break;
				}
			}
			name += std::to_string(count);
		}
		vec.emplace_back(new Priv{
			.w = that,
			.count = 0,
			.name = name,
			.guiBufferId = gui.setBuffer(typeName[0], kBufSize),
			.logger = nullptr,
			.type = typeName,
			.timestampMode = timestampMode,
			.firstTimestamp = 0,
			.relTimestampsOffset = getRelTimestampsOffset(typeSize),
			.countRelTimestamps = 0,
			.monitoring = kMonitorDont,
			.controlled = false,
		});
		Priv* p = vec.back();
		p->v.resize(kBufSize); // how do we include this above?
		if(((uintptr_t)p->v.data() + kMsgHeaderLength) & (typeSize - 1))
			throw(std::bad_alloc());
		p->maxCount = kTimestampBlock == p->timestampMode ? p->v.size() : p->relTimestampsOffset - (typeSize - 1);
		updateSometingToDo(p);
		return (Details*)vec.back();
	}

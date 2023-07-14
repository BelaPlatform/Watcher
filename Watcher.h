#include <vector>
#include <typeinfo>
#include <string>
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
	virtual void wmSet(double) = 0;
	virtual void localControlChanged() {}
protected:
	bool localControlEnabled = true;
};

#include <algorithm>
#include <vector>
#include <libraries/Gui/Gui.h>

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

class WatcherManager
{
public:
	WatcherManager(Gui& gui) : gui(gui) {
		gui.setControlDataCallback([this](JSONObject& json, void*) {
			this->controlCallback(json);
			return true;
		});
	};
	class Details;
	typedef float T;
	Details* reg(WatcherBase* that, const std::string& name)
	{
		constexpr size_t kBufSize = 4096;
		vec.emplace_back(Priv{
			.w = that,
			.count = 0,
			.name = name,
			.guiBufferId = gui.setBuffer(*typeid(T).name(), kBufSize),
			.watched = false,
			.controlled = false,
		});
		vec.back().v.resize(kBufSize); // how do we include this above?
		return (Details*)&vec.back();
	}
	void unreg(WatcherBase* that)
	{
		auto it = std::find_if(vec.begin(), vec.end(), [that](decltype(vec[0])& item){
			return item.w == that;
		});
		vec.erase(it);
		// TODO: unregister from GUI
	}
	// the relevant object is passed back here so that we don't have to waste
	// time looking it up
	template <typename T>
	void notify(Details* d, const T& value)
	{
		Priv* p = reinterpret_cast<Priv*>(d);
		if(p && p->watched)
		{
			((T*)p->v.data())[p->count++] = value;
			if(p->count == p->v.size() / sizeof(T))
			{
				send(p);
				p->count = 0;
			}
		}
	}
private:
	struct Priv {
		WatcherBase* w;
		std::vector<unsigned char> v;
		size_t count;
		std::string name;
		unsigned int guiBufferId;
		bool watched;
		bool controlled;
	};
	void send(Priv* p) {
		gui.sendBuffer(p->guiBufferId, (float*)p->v.data(), p->count / sizeof(float));
	}
	void startWatching(Priv* p) {
		if(p->watched)
			return;
		p->watched = true;
		p->count = 0;
		// TODO: register guiBufferId here
	}
	void stopWatching(Priv* p) {
		if(!p->watched)
			return;
		p->watched = false;
		// TODO: unregister guiBufferId here
	}
	void startControlling(Priv* p) {
		if(p->controlled)
			return;
		p->controlled = true;
		p->w->localControl(false);
	}
	void stopControlling(Priv* p) {
		if(!p->controlled)
			return;
		p->controlled = false;
		p->w->localControl(true);
	}

	Priv* findPrivByName(const std::string& str) {
		auto it = std::find_if(vec.begin(), vec.end(), [&str](decltype(vec[0])& item) {
			return item.name == str;
		});
		if(it != vec.end())
			return &*it;
		else
			return nullptr;
	}
	bool controlCallback(JSONObject& root) {
		auto watcher = JSONGetArray(root, "watcher");
		for(size_t n = 0; n < watcher.size(); ++n)
		{
			JSONValue* el = watcher[n];
			std::string cmd = JSONGetString(el, "cmd");
			printf("Command cmd: %s\n\r", cmd.c_str());
			if("list" == cmd)
			{
				printf("LIST\n");
				// send watcher list JSON
				JSONArray watchers;
				for(auto& v : vec)
				{
					JSONObject watcher;
					watcher[L"name"] = new JSONValue(JSON::s2ws(v.name));
					watcher[L"watched"] = new JSONValue(v.watched);
					watcher[L"controlled"] = new JSONValue(v.controlled);
					watcher[L"value"] = new JSONValue(v.w->wmGet());
					watchers.emplace_back(new JSONValue(watcher));
				}
				JSONObject watcher;
				watcher[L"watchers"] = new JSONValue(watchers);
				JSONObject root;
				root[L"watcher"] = new JSONValue(watcher);
				JSONValue value(root);
				gui.sendControl(&value);
			}
			if("watch" == cmd || "unwatch" == cmd || "control" == cmd || "uncontrol" == cmd) {
				const JSONArray& watchers = JSONGetArray(el, "watchers");
				for(size_t n = 0; n < watchers.size(); ++n)
				{
					std::string str = JSONGetAsString(watchers[n]);
					printf("'%s', ", str.c_str());
					Priv* p = findPrivByName(str);
					if(p)
					{
						if("watch" == cmd)
							startWatching(p);
						else if("unwatch" == cmd)
							stopWatching(p);
						else if("control" == cmd)
							startControlling(p);
						else if("uncontrol" == cmd)
							stopControlling(p);
					}
				}
				printf("\n");
			}
			if("set" == cmd) {
				const JSONArray& watchers = JSONGetArray(el, "watchers");
				const JSONArray& values = JSONGetArray(el, "values");
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
						p->w->wmSet(val);
					}
				}
			}
		}
		return false;
	}
	std::vector<Priv> vec;
	Gui& gui;
};

Gui gui; 
WatcherManager* Bela_getDefaultWatcherManager()
{
	static WatcherManager defaultWatcherManager(gui);
	return &defaultWatcherManager;
}

WatcherManager* Bela_getDefaultWatcherManager();
template <typename T>
class Watcher : public WatcherBase {
    static_assert(
	std::is_same<T,int>::value
	|| std::is_same<T,char>::value
	|| std::is_same<T,float>::value
	, "T is not of a supported type");
public:
	Watcher() = default;
	Watcher(const std::string& name, WatcherManager* wm = Bela_getDefaultWatcherManager()) :
		wm(wm)
	{
		if(wm)
			d = wm->reg(this, name);
	}
	~Watcher() {
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
	void wmSet(double value) override
	{
		vr = value;
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
	T vr;
	WatcherManager* wm;
	WatcherManager::Details* d;
};


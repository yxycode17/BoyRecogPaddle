#include "BoyRecogPlugin.h"
#include "plugin_internal.hpp"
#include "model_verify.hpp"
#include "diagnostics.hpp"
#include <shlobj.h>
#include <sddl.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <cstring>

namespace {
struct Handle {
    HANDLE h=nullptr;
    Handle()=default;
    explicit Handle(HANDLE v):h(v){}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle(){reset();}
    void reset(HANDLE v=nullptr){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);h=v;}
};
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::shared_mutex lifecycle;
std::mutex recognition,configuration,logMutex;
const wchar_t* modelFiles[]={L"profile.json",L"det/inference.onnx",L"det/inference.yml",L"rec/inference.onnx",L"rec/inference.yml"};
struct State {
    PluginPaths paths;
    HINSTANCE instance=nullptr;
    Handle logLock,pipe,process,job;
    std::ofstream log;
    fs::path modelRoot;
    Selection loadedChoice;
    uint32_t request=0;
    void Stop() noexcept {
        pipe.reset();
        if(process.h&&WaitForSingleObject(process.h,2000)!=WAIT_OBJECT_0){
            if(job.h)TerminateJobObject(job.h,1);
            WaitForSingleObject(process.h,2000);
        }
        job.reset();process.reset();modelRoot.clear();
    }
    ~State(){Stop();}
};
// 显式 Uninit 清理；不在 DllMain/CRT 卸载锁内等待子进程。
State* state=nullptr;
std::wstring Wide(const std::string& text){
    Require(text.find('\0')==std::string::npos,"Embedded NUL in UTF8");
    if(text.empty())return {};
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    Require(size>0,"Invalid UTF8 response");std::wstring out(size,L'\0');
    Require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),size)==size,"UTF8 conversion failed");return out;
}
std::wstring Quote(const std::wstring& input){
    std::wstring out=L"\"";size_t slashes=0;
    for(auto c:input){if(c==L'\\'){++slashes;continue;}out.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;if(c==L'"')out+=L'\\';out+=c;}
    out.append(slashes*2,L'\\');return out+L'"';
}
void Put(std::vector<unsigned char>& bytes,uint32_t n){for(int i=0;i<4;++i)bytes.push_back(static_cast<unsigned char>(n>>(8*i)));}
struct Reader {
    const std::vector<unsigned char>& bytes;size_t pos=0;
    uint32_t Number(){Require(bytes.size()-pos>=4,"Truncated response");uint32_t n=0;for(int i=0;i<4;++i)n|=uint32_t(bytes[pos++])<<(i*8);return n;}
    std::string Text(){auto n=Number();Require(n<=bytes.size()-pos,"Truncated text");std::string s(reinterpret_cast<const char*>(bytes.data()+pos),n);pos+=n;return s;}
};
void WaitIo(OVERLAPPED& ov,ULONGLONG deadline,DWORD& count){
    auto now=GetTickCount64();DWORD wait=now<deadline?static_cast<DWORD>(deadline-now):0;
    HANDLE handles[]={ov.hEvent,state->process.h};
    DWORD result=WaitForMultipleObjects(2,handles,FALSE,wait);
    if(result!=WAIT_OBJECT_0){CancelIoEx(state->pipe.h,&ov);GetOverlappedResult(state->pipe.h,&ov,&count,TRUE);throw std::runtime_error("Worker disconnected or timed out");}
    Require(GetOverlappedResult(state->pipe.h,&ov,&count,FALSE)!=FALSE,"Pipe I/O failed");
}
void Transfer(bool write,void* data,size_t size,ULONGLONG deadline){
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));Require(event.h!=nullptr,"Cannot create I/O event");
    auto p=static_cast<unsigned char*>(data);
    while(size){
        Require(GetTickCount64()<deadline,"Request timed out");
        OVERLAPPED ov{};ov.hEvent=event.h;ResetEvent(event.h);DWORD count=0;
        DWORD length=static_cast<DWORD>(std::min(size,size_t{65536}));
        BOOL ok=write?WriteFile(state->pipe.h,p,length,&count,&ov):ReadFile(state->pipe.h,p,length,&count,&ov);
        if(!ok){Require(GetLastError()==ERROR_IO_PENDING,"Pipe disconnected");WaitIo(ov,deadline,count);}
        Require(count>0,"Empty pipe transfer");p+=count;size-=count;
    }
}
void Start(const fs::path& root,const Selection& choice){
    if(state->process.h&&state->modelRoot==root&&WaitForSingleObject(state->process.h,0)==WAIT_TIMEOUT)return;
    state->Stop();
    try{
        GUID guid{};Require(SUCCEEDED(CoCreateGuid(&guid)),"Cannot create pipe identifier");wchar_t id[40]{};StringFromGUID2(guid,id,40);
        std::wstring name=L"\\\\.\\pipe\\BoyRecogPaddle-"+std::to_wstring(GetCurrentProcessId())+L"-"+id;
        Handle token;Require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h)!=FALSE,"Cannot read user token");
        DWORD length=0;GetTokenInformation(token.h,TokenUser,nullptr,0,&length);std::vector<unsigned char> user(length);
        Require(GetTokenInformation(token.h,TokenUser,user.data(),length,&length)!=FALSE,"Cannot read user SID");
        LPWSTR sid=nullptr;Require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&sid)!=FALSE,"Cannot encode SID");
        std::wstring acl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"Cannot create pipe ACL");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
        state->pipe.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&attributes));
        LocalFree(descriptor);Require(state->pipe.h!=INVALID_HANDLE_VALUE,"Cannot create private pipe");
        state->job.reset(CreateJobObjectW(nullptr,nullptr));Require(state->job.h!=nullptr,"Cannot create worker job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(SetInformationJobObject(state->job.h,JobObjectExtendedLimitInformation,&limit,sizeof(limit))!=FALSE,"Cannot configure worker job");
        auto exe=state->paths.install/L"worker/PaddleOcrWorker.exe";
        auto command=Quote(exe.wstring())+L" --pipe "+Quote(name)+L" --parent-pid "+std::to_wstring(GetCurrentProcessId())+
            L" --model-dir "+Quote(root.wstring())+L" --family "+Quote(choice.family)+L" --tier "+Quote(choice.tier)+L" --device auto --config-dir "+Quote(state->paths.config.wstring());
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION info{};
        Require(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,exe.parent_path().c_str(),&startup,&info)!=FALSE,"Cannot start bundled worker");
        state->process.reset(info.hProcess);Handle thread(info.hThread);
        if(!AssignProcessToJobObject(state->job.h,info.hProcess)){TerminateProcess(info.hProcess,1);throw std::runtime_error("Cannot assign worker job");}
        Require(ResumeThread(thread.h)!=DWORD(-1),"Cannot resume worker");
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));Require(event.h!=nullptr,"Cannot create connect event");
        OVERLAPPED ov{};ov.hEvent=event.h;DWORD count=0;
        if(!ConnectNamedPipe(state->pipe.h,&ov)){
            DWORD error=GetLastError();if(error==ERROR_IO_PENDING)WaitIo(ov,GetTickCount64()+10000,count);else Require(error==ERROR_PIPE_CONNECTED,"Cannot connect worker");
        }
        ULONG client=0;Require(GetNamedPipeClientProcessId(state->pipe.h,&client)&&client==info.dwProcessId,"Worker pipe identity mismatch");
        state->modelRoot=root;PluginLog("Worker started pid="+std::to_string(info.dwProcessId));
    }catch(...){state->Stop();throw;}
}
struct Item { BoyRecogRect box;std::wstring text; };
std::vector<Item> Recognize(const wchar_t* engine,const char* image,size_t size,ULONGLONG queueMs,bool& diagnostics){
    const auto requestStart=GetTickCount64();
    Require(engine&&std::wcscmp(engine,L"PaddleOCR")==0,"Unknown engine");
    Require(image&&size>=24&&size<=64*1024*1024,"Invalid image size");
    Require(std::memcmp(image,"\x89PNG\r\n\x1a\n",8)==0&&std::memcmp(image+12,"IHDR",4)==0,"PNG input required");
    auto be=[&](size_t offset){uint32_t n=0;for(size_t i=offset;i<offset+4;++i)n=(n<<8)|static_cast<unsigned char>(image[i]);return n;};
    uint32_t width=be(16),height=be(20);
    Require(width&&height&&width<=16384&&height<=16384&&uint64_t(width)*height<=64*1024*1024,"Image dimensions exceed limit");
    auto choice=ReadSelection(state->paths);auto& model=FindModel(choice);
    diagnostics=choice.diagnostics;
    auto preparationStart=DiagnosticClock::now();bool loadedNow=false;
    if(!state->process.h||WaitForSingleObject(state->process.h,0)!=WAIT_TIMEOUT||
       choice.family!=state->loadedChoice.family||choice.tier!=state->loadedChoice.tier){
        state->Stop();auto validationStart=GetTickCount64();
        auto root=ResolveModel(state->paths,model);
        PluginLog("Model validated for load ms="+std::to_string(GetTickCount64()-validationStart));
        Start(root,choice);state->loadedChoice=choice;
        loadedNow=true;
    }
    try{
        auto id=++state->request;std::vector<unsigned char> header;Put(header,0x31525042);Put(header,1);Put(header,id);Put(header,static_cast<uint32_t>(size));
        auto diagnostic=[&](const std::string& message){PluginLog("DIAG request_id="+std::to_string(id)+" "+message);};
        if(choice.diagnostics)diagnostic("stage=request model="+std::string(model.family)+"/"+model.tier+" image="+std::to_string(width)+"x"+std::to_string(height)+
            " bytes="+std::to_string(size)+" queue_ms="+std::to_string(queueMs)+" worker_started="+(loadedNow?"1":"0")+" prepare_ms="+Milliseconds(preparationStart));
        auto deadline=GetTickCount64()+125000;
        auto sendStart=DiagnosticClock::now();
        Transfer(true,header.data(),header.size(),deadline);Transfer(true,const_cast<char*>(image),size,deadline);
        auto sent=DiagnosticClock::now();
        Transfer(false,header.data(),header.size(),deadline);Reader h{header};
        Require(h.Number()==0x31525042&&h.Number()==0x80000001&&h.Number()==id,"Invalid response header");
        auto length=h.Number();Require(length>=20&&length<=8*1024*1024,"Invalid response size");
        std::vector<unsigned char> payload(length);Transfer(false,payload.data(),payload.size(),deadline);Reader r{payload};
        auto received=DiagnosticClock::now();
        auto status=r.Number(),w=r.Number(),ht=r.Number(),count=r.Number();
        Require(status<=1&&count<=(length-20)/24,"Invalid response status/count");
        Require(status?count==0:(w==width&&ht==height),"Response image dimensions mismatch");
        std::vector<Item> items;items.reserve(count);
        for(uint32_t i=0;i<count;++i){
            Require(r.Number()==3,"Unsupported result type");auto l=r.Number(),t=r.Number(),right=r.Number(),bottom=r.Number();
            Require(l<right&&t<bottom&&right<=width&&bottom<=height,"Invalid original-image rectangle");
            items.push_back({{static_cast<int32_t>(l),static_cast<int32_t>(t),static_cast<int32_t>(right),static_cast<int32_t>(bottom)},Wide(r.Text())});
        }
        auto error=r.Text();Require(r.pos==payload.size(),"Trailing response bytes");
        Require(status==0&&error.empty(),"Worker recognition failed; see worker log");
        if(choice.diagnostics)diagnostic("stage=roundtrip send_ms="+Milliseconds(sendStart,sent)+" wait_receive_ms="+Milliseconds(sent,received)+" parse_ms="+Milliseconds(received));
        PluginLog("OCR completed items="+std::to_string(items.size())+" total_ms="+std::to_string(GetTickCount64()-requestStart));return items;
    }catch(...){state->Stop();throw;}
}
template<class F> int Invoke(F call){
    try{std::shared_lock<std::shared_mutex> lock(lifecycle);Require(state!=nullptr,"Plugin not initialized");return call();}
    catch(const std::exception& e){PluginLog(e.what());return 1;}catch(...){PluginLog("Unhandled plugin exception");return 1;}
}
}

void PluginLog(const std::string& message) noexcept {
    try{std::lock_guard<std::mutex> lock(logMutex);if(state){SYSTEMTIME t{};GetLocalTime(&t);state->log<<t.wYear<<'-'<<t.wMonth<<'-'<<t.wDay<<' '<<t.wHour<<':'<<t.wMinute<<':'<<t.wSecond<<' '<<message<<'\n';state->log.flush();}}catch(...){}
}
const ModelHashes& FindModel(const Selection& selection){
    for(auto& m:kModels)if(selection.family==Wide(m.family)&&selection.tier==Wide(m.tier))return m;
    throw std::runtime_error("Unsupported model selection");
}
Selection ReadSelection(const PluginPaths& paths){
    std::lock_guard<std::mutex> lock(configuration);auto file=paths.config/L"settings.ini";
    wchar_t family[64]{},tier[64]{};
    GetPrivateProfileStringW(L"PaddleOCR",L"family",L"PP-OCRv6",family,64,file.c_str());
    GetPrivateProfileStringW(L"PaddleOCR",L"tier",L"small",tier,64,file.c_str());
    Selection result{family,tier,GetPrivateProfileIntW(L"PaddleOCR",L"diagnostics",0,file.c_str())!=0};FindModel(result);return result;
}
void SaveSelection(const PluginPaths& paths,const Selection& selection){
    const auto& m=FindModel(selection);std::lock_guard<std::mutex> lock(configuration);
    auto target=paths.config/L"settings.ini",temp=paths.config/(L"settings."+std::to_wstring(GetCurrentProcessId())+L".tmp");
    std::ofstream out(temp,std::ios::binary|std::ios::trunc);Require(bool(out),"Cannot write settings");
    out<<"[PaddleOCR]\r\nfamily="<<m.family<<"\r\ntier="<<m.tier<<"\r\ndiagnostics="<<(selection.diagnostics?1:0)<<"\r\n";out.close();Require(bool(out),"Cannot flush settings");
    Require(MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot replace settings");
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,target.c_str());PluginLog("Settings saved: "+std::string(m.family)+"/"+m.tier+" diagnostics="+(selection.diagnostics?"1":"0"));
}
fs::path DownloadRoot(const PluginPaths& paths,const ModelHashes& m){return paths.config/L"models"/fs::u8path(m.family)/fs::u8path(m.tier)/fs::u8path(m.revision);}
void VerifyPair(const fs::path& root,const ModelHashes& m){
    // 先检查完整性，再读取较大的权重，缺文件时立即失败。
    const char* hashes[]={m.profile,m.det,m.det_config,m.rec,m.rec_config};
    for(auto f:modelFiles)Require(fs::is_regular_file(root/f),"Missing model file; download from settings");
    for(size_t i=0;i<5;++i)VerifyModelFile(root/modelFiles[i],hashes[i]);
}
fs::path ResolveModel(const PluginPaths& paths,const ModelHashes& model){
    auto downloaded=DownloadRoot(paths,model);
    if(fs::exists(downloaded))try{VerifyPair(downloaded,model);return downloaded;}catch(const std::exception&){PluginLog("Configured model invalid; checking same installed model");}
    auto installed=paths.install/L"models"/fs::u8path(model.family)/fs::u8path(model.tier);VerifyPair(installed,model);return installed;
}
extern "C" int __cdecl BoyRecogInit(){
    try{
        std::unique_lock<std::shared_mutex> lock(lifecycle);if(state)return 0;
        auto next=std::make_unique<State>();
        Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&BoyRecogInit),&next->instance)!=FALSE,"Cannot locate plugin module");
        wchar_t filename[32768]{};auto n=GetModuleFileNameW(next->instance,filename,32768);Require(n&&n<32768,"Cannot locate plugin directory");
        next->paths.install=fs::path(filename).parent_path();PWSTR documents=nullptr;
        Require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&documents)),"Cannot resolve Documents folder");
        next->paths.config=fs::path(documents)/L"BoyPcReader/plugins/Recognition"/next->paths.install.filename();CoTaskMemFree(documents);
        auto logs=next->paths.config/L"logs";fs::create_directories(logs);
        next->logLock.reset(CreateFileW((logs/L"plugin.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(next->logLock.h==INVALID_HANDLE_VALUE){logs/=L"instances";logs/=std::to_wstring(GetCurrentProcessId());fs::create_directories(logs);
            next->logLock.reset(CreateFileW((logs/L"plugin.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));}
        Require(next->logLock.h!=INVALID_HANDLE_VALUE,"Cannot lock plugin log");
        auto current=logs/L"plugin.log";if(fs::exists(current))Require(MoveFileExW(current.c_str(),(logs/L"plugin.previous.log").c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE,"Cannot rotate plugin log");
        next->log.open(current,std::ios::binary|std::ios::trunc);Require(bool(next->log),"Cannot open plugin log");
        {std::lock_guard<std::mutex> guard(logMutex);state=next.release();}PluginLog("Plugin initialized; worker remains stopped");return 0;
    }catch(...){return 1;}
}
extern "C" int __cdecl BoyRecogUninit(){
    try{std::unique_lock<std::shared_mutex> lock(lifecycle);if(!state)return 0;state->Stop();PluginLog("Plugin uninitialized");
        std::lock_guard<std::mutex> guard(logMutex);delete state;state=nullptr;return 0;}catch(...){return 1;}
}
extern "C" int __cdecl BoyRecogGetEngineList(BoyRecogEngineListCallback callback,void* data){
    return Invoke([&]{Require(callback!=nullptr,"Missing callback");BoyRecogEngineInfo info{L"PaddleOCR",1|2|4|32};callback(&info,1,data);return 0;});
}
extern "C" int __cdecl BoyRecogOcrText(const wchar_t* engine,const char* image,size_t size,BoyRecogTextCallback callback,void* data){
    return Invoke([&]{Require(callback!=nullptr,"Missing callback");auto queueStart=GetTickCount64();std::lock_guard<std::mutex> lock(recognition);bool diagnostics=false;
        auto items=Recognize(engine,image,size,GetTickCount64()-queueStart,diagnostics);auto deliveryStart=DiagnosticClock::now();
        std::wstring text;for(auto& item:items){if(!text.empty())text+=L"\r\n";text+=item.text;}callback(text.c_str(),data);
        if(diagnostics)PluginLog("DIAG request_id="+std::to_string(state->request)+" stage=callback ms="+Milliseconds(deliveryStart));return 0;});
}
extern "C" int __cdecl BoyRecogOcrItems(const wchar_t* engine,const char* image,size_t size,int other,BoyRecogItemCallback callback,void* data){
    return Invoke([&]{Require(callback&&other==0,"Invalid callback or unsupported other types");auto queueStart=GetTickCount64();std::lock_guard<std::mutex> lock(recognition);bool diagnostics=false;
        auto items=Recognize(engine,image,size,GetTickCount64()-queueStart,diagnostics);auto deliveryStart=DiagnosticClock::now();
        for(auto& item:items)callback(3,item.text.c_str(),&item.box,nullptr,0,data);
        if(diagnostics)PluginLog("DIAG request_id="+std::to_string(state->request)+" stage=callback ms="+Milliseconds(deliveryStart));return 0;});
}
extern "C" int __cdecl BoyRecogSettings(const wchar_t* engine){
    return Invoke([&]{Require(engine&&std::wcscmp(engine,L"PaddleOCR")==0,"Unknown engine");return ShowSettings(state->paths,state->instance);});
}

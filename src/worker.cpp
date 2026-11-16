#include <windows.h>
#include <psapi.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <array>
#include "ocr.hpp"
#include "diagnostics.hpp"

namespace fs=std::filesystem;
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    Handle()=default; Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
};

static std::string Utf8(const std::wstring& text){
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(!n&&!text.empty())throw std::runtime_error("Invalid UTF16 argument");
    std::string out(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);return out;
}

static std::string JsonString(const std::string& text){
    std::string out="\"";
    for(unsigned char c:text){
        if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
        else if(c<32){out+="\\u00";out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}
        else out+=static_cast<char>(c);
    }
    return out+'"';
}

struct Logger {
    Handle lock; std::ofstream file; std::mutex mutex;
    explicit Logger(fs::path directory){
        fs::create_directories(directory);
        lock.value=CreateFileW((directory/L"worker.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lock.value==INVALID_HANDLE_VALUE){
            directory/=L"instances";directory/=std::to_wstring(GetCurrentProcessId());fs::create_directories(directory);
            lock.value=CreateFileW((directory/L"worker.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(lock.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot lock worker log");
        }
        const auto current=directory/L"worker.log",previous=directory/L"worker.previous.log";
        if(fs::exists(current)&&!MoveFileExW(current.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING))throw std::runtime_error("Log rotation failed");
        file.open(current,std::ios::binary|std::ios::trunc);if(!file)throw std::runtime_error("Cannot create log");
    }
    void Write(const std::string& message){
        std::lock_guard<std::mutex> guard(mutex);SYSTEMTIME t{};GetLocalTime(&t);
        file<<t.wYear<<'-'<<t.wMonth<<'-'<<t.wDay<<' '<<t.wHour<<':'<<t.wMinute<<':'<<t.wSecond<<" "<<message<<'\n';file.flush();
    }
};

static uint32_t Get(const unsigned char* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void Put(std::vector<unsigned char>& b,uint32_t n){for(int i=0;i<4;++i)b.push_back(static_cast<unsigned char>(n>>(8*i)));}
static void Text(std::vector<unsigned char>& b,const std::string& s){Put(b,static_cast<uint32_t>(s.size()));b.insert(b.end(),s.begin(),s.end());}
static bool Read(HANDLE pipe,unsigned char* data,size_t size){
    while(size){DWORD count=0;if(!ReadFile(pipe,data,static_cast<DWORD>(std::min(size,size_t{65536})),&count,nullptr)||!count)return false;data+=count;size-=count;}return true;
}
static void Write(HANDLE pipe,const std::vector<unsigned char>& data){
    size_t offset=0;while(offset<data.size()){DWORD count=0;if(!WriteFile(pipe,data.data()+offset,static_cast<DWORD>(std::min(data.size()-offset,size_t{65536})),&count,nullptr)||!count)throw std::runtime_error("Pipe write failed");offset+=count;}
}
static std::vector<unsigned char> Encode(const OcrResult& result,uint32_t status,const std::string& error){
    std::vector<unsigned char> b;Put(b,status);Put(b,result.width);Put(b,result.height);Put(b,static_cast<uint32_t>(result.items.size()));
    for(const auto& item:result.items){Put(b,3);Put(b,item.rect.x);Put(b,item.rect.y);Put(b,item.rect.x+item.rect.width);Put(b,item.rect.y+item.rect.height);Text(b,item.text);}
    Text(b,error);if(b.size()>8*1024*1024)throw std::runtime_error("Response exceeds limit");return b;
}
static bool SameUser(HANDLE process){
    Handle a,b;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&a.value)||!OpenProcessToken(process,TOKEN_QUERY,&b.value))return false;
    DWORD na=0,nb=0;GetTokenInformation(a.value,TokenUser,nullptr,0,&na);GetTokenInformation(b.value,TokenUser,nullptr,0,&nb);
    std::vector<unsigned char> va(na),vb(nb);
    if(!GetTokenInformation(a.value,TokenUser,va.data(),na,&na)||!GetTokenInformation(b.value,TokenUser,vb.data(),nb,&nb))return false;
    return EqualSid(reinterpret_cast<TOKEN_USER*>(va.data())->User.Sid,reinterpret_cast<TOKEN_USER*>(vb.data())->User.Sid)!=FALSE;
}

int wmain(int argc,wchar_t** argv){
    try{
        if(!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32))throw std::runtime_error("Cannot restrict DLL search path");
        wchar_t filename[32768]{};DWORD length=GetModuleFileNameW(nullptr,filename,32768);
        if(!length||length>=32768)throw std::runtime_error("Cannot locate worker");
        const fs::path exeDirectory=fs::path(filename).parent_path();
        // 所有第三方导入均 delay-load；进入 main 后按绝对路径锁定本目录运行库。
#ifdef _DEBUG
        const wchar_t* opencv=L"opencv_world500d.dll";
#else
        const wchar_t* opencv=L"opencv_world500.dll";
#endif
        for(const wchar_t* library:{L"DirectML.dll",L"onnxruntime.dll",opencv})
            if(!LoadLibraryExW((exeDirectory/library).c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32))
                throw std::runtime_error("Required bundled runtime cannot be loaded");
        std::map<std::wstring,std::wstring> args;
        for(int i=1;i<argc;i+=2){if(i+1>=argc||std::wstring(argv[i]).rfind(L"--",0)!=0||!args.emplace(argv[i],argv[i+1]).second)throw std::runtime_error("Invalid arguments");}
        const std::vector<std::wstring> allowed{L"--model-dir",L"--family",L"--tier",L"--device",L"--pipe",L"--parent-pid",L"--config-dir",L"--image",L"--output"};
        for(const auto& [key,value]:args)if(std::find(allowed.begin(),allowed.end(),key)==allowed.end())throw std::runtime_error("Unknown argument");
        auto get=[&](const wchar_t* key,const wchar_t* fallback){auto it=args.find(key);return it==args.end()?std::wstring(fallback):it->second;};
        fs::path config=get(L"--config-dir",L"");
        if(config.empty()){
            PWSTR documents=nullptr;auto hr=SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&documents);
            if(FAILED(hr))throw std::runtime_error("Cannot resolve Documents folder");
            config=fs::path(documents)/L"BoyPcReader/plugins/Recognition"/exeDirectory.parent_path().filename();CoTaskMemFree(documents);
        }
        if(!config.is_absolute())throw std::runtime_error("Config directory must be absolute");
        uint32_t activeRequest=0;
        Logger logger(config/L"logs");auto log=[&](const std::string& s){
            logger.Write(s.rfind("DIAG ",0)==0?"DIAG request_id="+std::to_string(activeRequest)+" "+s.substr(5):s);
        };
        auto diagnosticsEnabled=[&]{return GetPrivateProfileIntW(L"PaddleOCR",L"diagnostics",0,(config/L"settings.ini").c_str())!=0;};
        log("Worker started pid="+std::to_string(GetCurrentProcessId()));
        log("OpenCV runtime="+cv::getVersionString());
        auto family=Utf8(get(L"--family",L"PP-OCRv6")),tier=Utf8(get(L"--tier",L"small")),mode=Utf8(get(L"--device",L"auto"));
        fs::path root=get(L"--model-dir",L"");
        if(root.empty())root=exeDirectory.parent_path()/L"models"/fs::u8path(family)/fs::u8path(tier);
        if(!root.is_absolute())throw std::runtime_error("Model directory must be absolute");
        std::unique_ptr<Ocr> ocr;
        auto recognize=[&](const std::vector<unsigned char>& png,bool diagnostics){
            auto loadStart=DiagnosticClock::now();bool loading=!ocr;
            if(!ocr)ocr=std::make_unique<Ocr>(root,family,tier,mode,log,diagnostics);
            else ocr->SetDiagnostics(diagnostics);
            if(diagnostics)log("DIAG stage=model_ready loaded_now="+std::string(loading?"1":"0")+" ms="+Milliseconds(loadStart));
            return ocr->Recognize(png);
        };
        auto pipeName=get(L"--pipe",L"");
        if(pipeName.empty()){
            auto imagePath=get(L"--image",L""),outputPath=get(L"--output",L"");
            if(imagePath.empty()||outputPath.empty())throw std::runtime_error("Use --pipe and --parent-pid, or --image and --output for development");
            if(fs::file_size(imagePath)>64*1024*1024)throw std::runtime_error("Image file too large");
            std::ifstream file(imagePath,std::ios::binary);std::vector<unsigned char> png{std::istreambuf_iterator<char>(file),{}};
            auto start=std::chrono::steady_clock::now();auto result=recognize(png,diagnosticsEnabled());
            std::ofstream output(fs::path(outputPath),std::ios::binary);
            if(!output)throw std::runtime_error("Cannot write development output");
            output<<"{\"width\":"<<result.width<<",\"height\":"<<result.height<<",\"items\":[";
            bool first=true;
            for(const auto& item:result.items){
                if(!first)output<<',';first=false;
                output<<"{\"text\":"<<JsonString(item.text)<<",\"score\":"<<item.confidence<<",\"rect\":["<<item.rect.x<<','<<item.rect.y<<','<<item.rect.x+item.rect.width<<','<<item.rect.y+item.rect.height<<"],\"quad\":[";
                for(size_t i=0;i<item.quad.size();++i){if(i)output<<',';output<<'['<<static_cast<int>(item.quad[i].x)<<','<<static_cast<int>(item.quad[i].y)<<']';}
                output<<"]}";
            }
            output<<"]}";output.close();if(!output)throw std::runtime_error("Development output write failed");
            std::cout<<"items="<<result.items.size()<<" total_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<<'\n';
            log("Development image completed items="+std::to_string(result.items.size()));return 0;
        }
        if(pipeName.rfind(L"\\\\.\\pipe\\BoyRecogPaddle-",0)!=0||pipeName.size()>240)throw std::runtime_error("Invalid local pipe name");
        size_t parsed=0;auto pidText=get(L"--parent-pid",L"");auto pidValue=std::stoul(pidText,&parsed);
        if(parsed!=pidText.size()||!pidValue)throw std::runtime_error("Invalid parent PID");
        Handle parent;parent.value=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pidValue);
        if(!parent.value||!SameUser(parent.value))throw std::runtime_error("Parent identity mismatch");
        Handle pipe;
        if(!WaitNamedPipeW(pipeName.c_str(),5000))throw std::runtime_error("Pipe connection timeout");
        pipe.value=CreateFileW(pipeName.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        ULONG serverPid=0;
        if(pipe.value==INVALID_HANDLE_VALUE||!GetNamedPipeServerProcessId(pipe.value,&serverPid)||serverPid!=pidValue)throw std::runtime_error("Pipe server identity mismatch");
        log("Pipe connected");
        std::atomic<bool> stop{false};std::atomic<ULONGLONG> deadline{0};
        std::thread watcher([&]{
            while(!stop.load()){
                const auto due=deadline.load();
                if(WaitForSingleObject(parent.value,0)==WAIT_OBJECT_0||!PeekNamedPipe(pipe.value,nullptr,0,nullptr,nullptr,nullptr)||(due&&GetTickCount64()>due)){
                    log("Parent/pipe disconnected or request deadline exceeded; exiting");
                    // ONNX/驱动可能阻塞，独立监测线程保证断管后有界退出，不等待下一次请求。
                    TerminateProcess(GetCurrentProcess(),due&&GetTickCount64()>due?3:0);return;
                }
                Sleep(50);
            }
        });
        try{
            for(;;){
                std::array<unsigned char,16> header{};if(!Read(pipe.value,header.data(),1))break;
                deadline=GetTickCount64()+120000;
                if(!Read(pipe.value,header.data()+1,header.size()-1))break;
                uint32_t magic=Get(header.data()),operation=Get(header.data()+4),id=Get(header.data()+8),size=Get(header.data()+12);
                if(magic!=0x31525042||size>64*1024*1024||operation<1||operation>3||(operation!=1&&size))throw std::runtime_error("Invalid pipe frame");
                activeRequest=id;bool diagnostics=operation==1&&diagnosticsEnabled();auto receivedStart=DiagnosticClock::now();
                deadline=GetTickCount64()+120000;
                { // 请求缓冲区在记录驻留内存前销毁；保留模型和推理缓存。
                    std::vector<unsigned char> png(size);if(!Read(pipe.value,png.data(),png.size()))break;
                    if(diagnostics)log("DIAG stage=pipe_receive bytes="+std::to_string(size)+" ms="+Milliseconds(receivedStart));
                    OcrResult result;uint32_t status=0;std::string error;
                    auto ocrStart=DiagnosticClock::now();
                    if(operation==1){
                        try{result=recognize(png,diagnostics);log("OCR completed items="+std::to_string(result.items.size()));}
                        catch(const std::exception& e){status=1;error=e.what();log("OCR failed: "+error);}
                    }
                    if(diagnostics)log("DIAG stage=ocr_total ms="+Milliseconds(ocrStart)+" status="+std::to_string(status));
                    auto encodeStart=DiagnosticClock::now();
                    auto body=Encode(result,status,error);std::vector<unsigned char> response;
                    Put(response,0x31525042);Put(response,operation|0x80000000u);Put(response,id);Put(response,static_cast<uint32_t>(body.size()));
                    response.insert(response.end(),body.begin(),body.end());
                    if(diagnostics)log("DIAG stage=response_encode ms="+Milliseconds(encodeStart)+" bytes="+std::to_string(response.size()));
                    auto writeStart=DiagnosticClock::now();Write(pipe.value,response);deadline=0;
                    if(diagnostics)log("DIAG stage=pipe_send ms="+Milliseconds(writeStart));
                }
                if(diagnostics){
                    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
                    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)))
                        log("DIAG stage=request_memory working_set_bytes="+std::to_string(memory.WorkingSetSize)+
                            " private_commit_bytes="+std::to_string(memory.PrivateUsage));
                    else log("DIAG stage=request_memory error="+std::to_string(GetLastError()));
                }
                if(operation==3)break;
            }
        }catch(const std::exception& e){log("Pipe failure: "+std::string(e.what()));stop=true;watcher.join();throw;}
        stop=true;watcher.join();log("Pipe closed; worker exiting");return 0;
    }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}

// x86 ABI 集成检查：真实加载插件，回调期间复制结果，不跨 DLL 释放内存。
#include "../src/BoyRecogPlugin.h"
#include "model_hashes.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <cassert>
#include <algorithm>
namespace fs=std::filesystem;
static_assert(sizeof(void*)==4);
static_assert(sizeof(BoyRecogEngineInfo)==16&&offsetof(BoyRecogEngineInfo,capabilities)==8);
static_assert(sizeof(BoyRecogRect)==16&&sizeof(BoyRecogCharInfo)==20);
std::vector<DWORD> Children(){
    std::vector<DWORD> ids;HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);assert(snap!=INVALID_HANDLE_VALUE);
    PROCESSENTRY32W entry{sizeof(entry)};
    if(Process32FirstW(snap,&entry))do{if(entry.th32ParentProcessID==GetCurrentProcessId()&&std::wstring(entry.szExeFile)==L"PaddleOcrWorker.exe")ids.push_back(entry.th32ProcessID);}while(Process32NextW(snap,&entry));
    CloseHandle(snap);return ids;
}
void KillWorker(){
    auto ids=Children();assert(ids.size()==1);HANDLE worker=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE,FALSE,ids[0]);assert(worker);
    assert(TerminateProcess(worker,7));assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0);CloseHandle(worker);
}
struct Output {DWORD thread=GetCurrentThreadId();int calls=0;std::wstring text;std::vector<BoyRecogRect> boxes;};
void __cdecl Text(const wchar_t* text,void* ptr){auto& out=*static_cast<Output*>(ptr);assert(GetCurrentThreadId()==out.thread&&text);++out.calls;out.text=text;}
void __cdecl Item(BoyRecogItemType type,const wchar_t* text,const BoyRecogRect* box,const BoyRecogCharInfo* chars,size_t count,void* ptr){
    auto& out=*static_cast<Output*>(ptr);assert(GetCurrentThreadId()==out.thread&&type==3&&text&&box&&!chars&&!count);
    assert(box->left>=0&&box->top>=0&&box->left<box->right&&box->top<box->bottom);
    if(out.calls++)out.text+=L"\r\n";out.text+=text;out.boxes.push_back(*box);
}
void __cdecl Engines(const BoyRecogEngineInfo* info,size_t count,void* ptr){auto& out=*static_cast<Output*>(ptr);assert(count==1&&std::wstring(info->name)==L"PaddleOCR"&&info->capabilities==39);assert(out.thread==GetCurrentThreadId());++out.calls;}
template<class T>T Load(HMODULE dll,const char* name){auto p=GetProcAddress(dll,name);assert(p);return reinterpret_cast<T>(p);}
void LinkTree(const fs::path& source,const fs::path& target){
    fs::create_directories(target);for(auto& e:fs::recursive_directory_iterator(source)){auto dest=target/fs::relative(e.path(),source);if(e.is_directory())fs::create_directories(dest);else if(e.path().extension()==L".onnx"&&source.root_name()==target.root_name())fs::create_hard_link(e.path(),dest);else fs::copy_file(e.path(),dest);}
}
int wmain(int argc,wchar_t** argv){
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    auto defaultDist=(fs::path(executable).parent_path().parent_path().parent_path()/L"dist").wstring();
    wchar_t* interactive[]={executable,defaultDist.data(),executable,const_cast<wchar_t*>(L"--settings")};
    if(argc==1){argc=4;argv=interactive;}
    assert(argc>=3);auto source=fs::absolute(argv[1]);auto image=fs::absolute(argv[2]);
    auto install=source.parent_path()/L"build/plugin-tests"/(L"PaddlePluginTest-"+std::to_wstring(GetCurrentProcessId()));
    LinkTree(source,install);PWSTR docs=nullptr;assert(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&docs)));
    auto config=fs::path(docs)/L"BoyPcReader/plugins/Recognition"/install.filename();CoTaskMemFree(docs);
    auto path=install/L"BoyRecogPaddle.dll";
    DWORD ignored=0;auto bytes=GetFileVersionInfoSizeW(path.c_str(),&ignored);assert(bytes);std::vector<char> version(bytes);assert(GetFileVersionInfoW(path.c_str(),0,bytes,version.data()));
    LPVOID value=nullptr;UINT len=0;assert(VerQueryValueW(version.data(),L"\\StringFileInfo\\080404b0\\ProductName",&value,&len));assert(std::wstring(static_cast<wchar_t*>(value))==L"百度飞桨");
    assert(VerQueryValueW(version.data(),L"\\StringFileInfo\\080404b0\\ProductVersion",&value,&len));assert(std::wstring(static_cast<wchar_t*>(value))==L"1.2");
    HMODULE dll=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);assert(dll);
    auto init=Load<decltype(&BoyRecogInit)>(dll,"BoyRecogInit"),uninit=Load<decltype(&BoyRecogUninit)>(dll,"BoyRecogUninit");
    auto list=Load<decltype(&BoyRecogGetEngineList)>(dll,"BoyRecogGetEngineList");
    auto text=Load<decltype(&BoyRecogOcrText)>(dll,"BoyRecogOcrText");auto items=Load<decltype(&BoyRecogOcrItems)>(dll,"BoyRecogOcrItems");
    auto settings=Load<decltype(&BoyRecogSettings)>(dll,"BoyRecogSettings");
    Output out;assert(list(Engines,&out)!=0&&out.calls==0);assert(settings(L"PaddleOCR")!=0);assert(init()==0&&init()==0);assert(Children().empty());
    assert(list(Engines,&out)==0&&out.calls==1);assert(list(nullptr,nullptr)!=0);assert(settings(L"invalid")!=0);
    assert(!GetModuleHandleW(L"onnxruntime.dll")&&!GetModuleHandleW(L"opencv_world500.dll")&&!GetModuleHandleW(L"DirectML.dll"));
    if(argc==4&&std::wstring(argv[3])==L"--settings"){
        std::wcout<<L"CONFIG="<<config<<std::endl;assert(settings(L"PaddleOCR")==0);assert(uninit()==0);FreeLibrary(dll);return 0;
    }
    std::ifstream input(image,std::ios::binary);std::string png{std::istreambuf_iterator<char>(input),{}};assert(!png.empty());
    Output invalid;assert(text(L"bad",png.data(),png.size(),Text,&invalid)!=0);assert(text(L"PaddleOCR",nullptr,0,Text,&invalid)!=0);
    assert(text(L"PaddleOCR","bad",3,Text,&invalid)!=0);assert(items(L"PaddleOCR",png.data(),png.size(),1,Item,&invalid)!=0);assert(items(L"PaddleOCR",png.data(),png.size(),-1,Item,&invalid)!=0);
    assert(text(L"PaddleOCR",png.data(),png.size(),nullptr,nullptr)!=0);assert(invalid.calls==0&&Children().empty());
    auto model=install/L"models/PP-OCRv6/small";fs::rename(model/L"profile.json",model/L"profile.saved");
    assert(text(L"PaddleOCR",png.data(),png.size(),Text,&invalid)!=0&&invalid.calls==0&&Children().empty());
    fs::rename(model/L"profile.saved",model/L"profile.json");
    Output first,second;
    std::thread a([&]{first.thread=GetCurrentThreadId();assert(items(L"PaddleOCR",png.data(),png.size(),0,Item,&first)==0);});
    std::thread b([&]{second.thread=GetCurrentThreadId();assert(text(L"PaddleOCR",png.data(),png.size(),Text,&second)==0);});a.join();b.join();
    assert(first.calls>0&&second.calls==1&&first.text==second.text);auto workers=Children();assert(workers.size()==1);
    Output repeated;assert(text(L"PaddleOCR",png.data(),png.size(),Text,&repeated)==0&&repeated.text==first.text);assert(Children()==workers);
    auto diagnosticsIni=config/L"settings.ini";assert(WritePrivateProfileStringW(L"PaddleOCR",L"diagnostics",L"1",diagnosticsIni.c_str()));
    Output diagnosed;assert(text(L"PaddleOCR",png.data(),png.size(),Text,&diagnosed)==0&&diagnosed.text==first.text);assert(Children()==workers);
    {std::ifstream log(config/L"logs/plugin.log");std::string lines{std::istreambuf_iterator<char>(log),{}};
        assert(lines.find("stage=roundtrip")!=std::string::npos&&lines.find("stage=callback")!=std::string::npos);}
    assert(WritePrivateProfileStringW(L"PaddleOCR",L"diagnostics",L"0",diagnosticsIni.c_str()));
    {std::ifstream log(config/L"logs/plugin.log");std::string lines{std::istreambuf_iterator<char>(log),{}};
        auto firstValidation=lines.find("Model validated for load ms=");assert(firstValidation!=std::string::npos);
        assert(lines.find("Model validated for load ms=",firstValidation+1)==std::string::npos);}
    // 切换模型必须结束旧进程，使旧模型及 CPU/GPU 推理缓存随进程释放。
    LinkTree(source.parent_path()/L"downloads/models/PP-OCRv6/tiny",install/L"models/PP-OCRv6/tiny");
    for(const wchar_t* tier:{L"tiny",L"small"}){
        HANDLE previous=OpenProcess(SYNCHRONIZE,FALSE,workers[0]);assert(previous);
        assert(WritePrivateProfileStringW(L"PaddleOCR",L"tier",tier,diagnosticsIni.c_str()));
        Output switched;assert(text(L"PaddleOCR",png.data(),png.size(),Text,&switched)==0&&switched.calls==1&&!switched.text.empty());
        assert(WaitForSingleObject(previous,0)==WAIT_OBJECT_0);CloseHandle(previous);
        auto current=Children();assert(current.size()==1&&current!=workers);workers=current;
        if(std::wstring(tier)==L"small")assert(switched.text==first.text);
    }
    KillWorker();
    Output restarted;assert(text(L"PaddleOCR",png.data(),png.size(),Text,&restarted)==0&&restarted.text==first.text);assert(Children().size()==1&&Children()!=workers);
    auto ini=config/L"settings.ini";assert(WritePrivateProfileStringW(L"PaddleOCR",L"tier",L"missing",ini.c_str()));
    Output badSetting;assert(text(L"PaddleOCR",png.data(),png.size(),Text,&badSetting)!=0&&badSetting.calls==0);
    assert(WritePrivateProfileStringW(L"PaddleOCR",L"tier",L"small",ini.c_str()));
    const ModelHashes* smallModel=nullptr;for(auto& m:kModels)if(std::string(m.family)=="PP-OCRv6"&&std::string(m.tier)=="small")smallModel=&m;assert(smallModel);
    auto downloaded=config/L"models/PP-OCRv6/small"/fs::u8path(smallModel->revision);LinkTree(model,downloaded);
    fs::rename(model/L"profile.json",model/L"profile.saved");
    KillWorker();
    Output preferred;assert(text(L"PaddleOCR",png.data(),png.size(),Text,&preferred)==0&&preferred.text==first.text);
    fs::rename(model/L"profile.saved",model/L"profile.json");
    {std::ofstream bad(downloaded/L"profile.json",std::ios::trunc);bad<<"corrupt";}
    KillWorker();
    Output fallback;assert(text(L"PaddleOCR",png.data(),png.size(),Text,&fallback)==0&&fallback.text==first.text);
    assert(uninit()==0&&Children().empty());assert(uninit()==0);assert(text(L"PaddleOCR",png.data(),png.size(),Text,&invalid)!=0);
    assert(init()==0&&Children().empty());assert(fs::exists(config/L"logs/plugin.previous.log"));assert(uninit()==0);assert(FreeLibrary(dll));
    std::cout<<"x86 ABI, lazy start, callbacks, concurrency, model switch process exit, worker restart, missing model, config, log rotation and unload passed; items="<<first.calls<<'\n';
    std::wcout<<L"Test artifacts: "<<install<<L" and "<<config<<'\n';
}

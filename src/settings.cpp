#include "plugin_internal.hpp"
#include "model_verify.hpp"
#include "settings_ids.h"
#include <winhttp.h>
#include <objbase.h>
#include <commctrl.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cstring>

namespace {
std::mutex settingsMutex;
constexpr UINT TaskCompleted=WM_APP+1;
struct Http {
    HINTERNET h=nullptr;
    explicit Http(HINTERNET value):h(value){if(!h)throw std::runtime_error("WinHTTP handle creation failed");}
    ~Http(){WinHttpCloseHandle(h);}
    Http(const Http&)=delete;
};
struct Dialog {
    PluginPaths paths;
    HWND window=nullptr;
    std::thread task;
    std::atomic<bool> cancel{false},done{false};
    bool save=false,success=false,needsDownload=false,downloadRequested=false;
    HINSTANCE instance=nullptr;
    std::mutex progressMutex;
    std::wstring status;
    unsigned percent=0;
    Selection choice;
    Selection originalChoice;
    bool originalValid=false;
    void Check(){if(cancel.load())throw std::runtime_error("Operation cancelled");}
    void Progress(std::wstring text,unsigned value){std::lock_guard<std::mutex> lock(progressMutex);status=std::move(text);percent=value;}
};
std::wstring Widen(const char* ascii){return std::wstring(ascii,ascii+std::strlen(ascii));}
const Selection choices[]={{L"PP-OCRv6",L"tiny"},{L"PP-OCRv6",L"small"},{L"PP-OCRv6",L"medium"},{L"PP-OCRv5",L"mobile"}};
bool Unchanged(const Dialog& dialog,const Selection& choice){
    return dialog.originalValid&&choice.family==dialog.originalChoice.family&&choice.tier==dialog.originalChoice.tier;
}
void Download(Dialog& dialog){
    dialog.Check();
    const auto& m=FindModel(dialog.choice);auto target=DownloadRoot(dialog.paths,m);
    fs::create_directories(target.parent_path());
    HANDLE lock=CreateFileW((target.parent_path()/L"download.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(lock==INVALID_HANDLE_VALUE)throw std::runtime_error("Another process is downloading this model");
    struct Unlock {HANDLE h;~Unlock(){CloseHandle(h);}} unlock{lock};
    try{VerifyPair(target,m);dialog.Progress(L"所选模型已下载并校验通过。",100);return;}catch(const std::exception&){}
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Cannot create download identifier");wchar_t id[40]{};StringFromGUID2(guid,id,40);
    auto staging=target.parent_path()/(std::wstring(L".download-")+id);
    fs::create_directory(staging);
    try{
        uint64_t total=0,completed=0;
        for(auto& f:kDownloads)if(std::strcmp(f.family,m.family)==0&&std::strcmp(f.tier,m.tier)==0)total+=f.size;
        dialog.Check();dialog.Progress(L"正在连接模型服务器……",0);
        Http session(WinHttpOpen(L"BoyRecogPaddle/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
        if(!WinHttpSetTimeouts(session.h,10000,10000,10000,10000))throw std::runtime_error("Cannot set HTTP timeout");
        for(auto& file:kDownloads){
            if(std::strcmp(file.family,m.family)||std::strcmp(file.tier,m.tier))continue;
            dialog.Check();auto url=Widen(file.url);URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
            if(!WinHttpCrackUrl(url.c_str(),static_cast<DWORD>(url.size()),0,&parts)||parts.nScheme!=INTERNET_SCHEME_HTTPS)throw std::runtime_error("Invalid HTTPS model URL");
            std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);
            if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
            Http connection(WinHttpConnect(session.h,host.c_str(),parts.nPort,0));
            Http request(WinHttpOpenRequest(connection.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
            DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
            if(!WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)))throw std::runtime_error("Cannot secure HTTP redirects");
            dialog.Progress(L"正在连接："+Widen(file.name),static_cast<unsigned>(completed*100/total));
            if(!WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.h,nullptr))throw std::runtime_error("Model HTTP request failed");
            DWORD status=0,size=sizeof(status);
            if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)||status!=200)throw std::runtime_error("Model server returned non-200 status");
            auto destination=staging/fs::u8path(file.name);fs::create_directories(destination.parent_path());
            std::ofstream output(destination,std::ios::binary|std::ios::trunc);if(!output)throw std::runtime_error("Cannot create downloaded model");
            std::array<char,65536> buffer{};uint64_t received=0;auto deadline=GetTickCount64()+30*60*1000;
            for(;;){
                dialog.Check();if(GetTickCount64()>deadline)throw std::runtime_error("Model download deadline exceeded");
                DWORD read=0;if(!WinHttpReadData(request.h,buffer.data(),static_cast<DWORD>(buffer.size()),&read))throw std::runtime_error("Model download read failed");
                if(!read)break;received+=read;if(received>file.size)throw std::runtime_error("Downloaded model exceeds expected size");
                output.write(buffer.data(),read);if(!output)throw std::runtime_error("Model write failed; check available disk space");
                unsigned percent=static_cast<unsigned>((completed+received)*100/total);
                std::wostringstream text;text<<Widen(file.name)<<L"："<<received<<L" / "<<file.size<<L" 字节（"<<received*100/file.size<<L"%）\r\n总进度："<<completed+received<<L" / "<<total<<L" 字节（"<<percent<<L"%）";
                dialog.Progress(text.str(),percent);
            }
            output.close();if(!output||received!=file.size)throw std::runtime_error("Downloaded model size mismatch");
            dialog.Check();dialog.Progress(L"正在校验："+Widen(file.name),static_cast<unsigned>((completed+received)*100/total));
            VerifyModelFile(destination,file.sha256);completed+=received;
        }
        dialog.Check();dialog.Progress(L"正在校验完整模型组合……",100);
        fs::copy_file(dialog.paths.install/L"profiles"/fs::u8path(m.family)/(std::string(m.tier)+".json"),staging/L"profile.json");
        VerifyPair(staging,m);dialog.Check();
        // 同目录重命名发布；失败恢复已有目录，绝不删除先前用户模型。
        fs::path backup=target.parent_path()/(std::wstring(L".previous-")+id);
        bool replaced=fs::exists(target);if(replaced)fs::rename(target,backup);
        try{fs::rename(staging,target);}catch(...){if(replaced)fs::rename(backup,target);throw;}
        dialog.Progress(L"模型下载并校验完成。",100);PluginLog("Model download verified: "+std::string(m.family)+"/"+m.tier);
    }catch(...){std::error_code ignored;fs::remove_all(staging,ignored);throw;}
}
void RunTask(Dialog& dialog,bool download){
    try{
        if(download)Download(dialog);
        if(dialog.save){
            try{ResolveModel(dialog.paths,FindModel(dialog.choice));}
            catch(const std::exception&){if(!download&&!dialog.cancel)dialog.needsDownload=true;throw;}
            dialog.Check();
        }
        dialog.success=true;
    }catch(const std::exception& e){PluginLog(e.what());dialog.Progress(dialog.cancel?L"操作已取消，原配置未修改。":L"操作失败：所选模型不可用，或网络、文件操作失败。原配置未修改，详情见 plugin.log。",0);}
    catch(...){dialog.Progress(L"操作失败，原配置未修改。",0);}
    dialog.done=true;
    if(dialog.window)PostMessageW(dialog.window,TaskCompleted,0,0);
}
void CommitSelection(Dialog& dialog){
    if(dialog.save&&dialog.success&&!dialog.cancel)SaveSelection(dialog.paths,dialog.choice);
}
void RefreshProgress(Dialog& dialog){
    std::wstring status;unsigned percent=0;
    {std::lock_guard<std::mutex> lock(dialog.progressMutex);status=dialog.status;percent=dialog.percent;}
    wchar_t old[1024]{};GetDlgItemTextW(dialog.window,IDC_STATUS,old,1024);
    if(status!=old)SetDlgItemTextW(dialog.window,IDC_STATUS,status.c_str());
    SendDlgItemMessageW(dialog.window,IDC_PROGRESS,PBM_SETPOS,percent,0);
    if(dialog.done.load())EndDialog(dialog.window,0);
}
INT_PTR CALLBACK ProgressProcedure(HWND window,UINT message,WPARAM w,LPARAM l){
    auto dialog=reinterpret_cast<Dialog*>(GetWindowLongPtrW(window,DWLP_USER));
    try{
        if(message==WM_INITDIALOG){
            dialog=reinterpret_cast<Dialog*>(l);dialog->window=window;SetWindowLongPtrW(window,DWLP_USER,l);
            SetWindowTextW(window,dialog->downloadRequested?L"下载模型":L"校验模型");
            SendDlgItemMessageW(window,IDC_PROGRESS,PBM_SETRANGE32,0,100);
            if(!SetTimer(window,1,300,nullptr))throw std::runtime_error("Cannot create progress timer");
            dialog->task=std::thread([dialog]{RunTask(*dialog,dialog->downloadRequested);});
            RefreshProgress(*dialog);return TRUE;
        }
        if(!dialog)return FALSE;
        if(message==WM_TIMER||message==TaskCompleted){RefreshProgress(*dialog);return TRUE;}
        if(message==WM_CLOSE||(message==WM_COMMAND&&LOWORD(w)==IDCANCEL)){
            dialog->cancel=true;dialog->Progress(L"正在取消，请等待当前操作结束……",0);
            EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);return TRUE;
        }
        if(message==WM_DESTROY){KillTimer(window,1);return TRUE;}
    }catch(const std::exception& e){PluginLog(e.what());if(dialog)dialog->cancel=true;EndDialog(window,1);return TRUE;}
    catch(...){if(dialog)dialog->cancel=true;EndDialog(window,1);return TRUE;}
    return FALSE;
}
void RunOperation(Dialog& dialog,HWND owner,bool download){
    dialog.save=true;dialog.downloadRequested=download;dialog.cancel=false;dialog.done=false;
    dialog.success=false;dialog.needsDownload=false;
    dialog.Progress(download?L"准备下载……":L"正在校验所选模型……",0);
    auto result=DialogBoxParamW(dialog.instance,MAKEINTRESOURCEW(IDD_PROGRESS),owner,ProgressProcedure,reinterpret_cast<LPARAM>(&dialog));
    if(result!=0)dialog.cancel=true;
    if(dialog.task.joinable())dialog.task.join();
    dialog.window=nullptr;
    if(result!=0)throw std::runtime_error("Progress dialog failed");
    CommitSelection(dialog);
}
INT_PTR CALLBACK Procedure(HWND window,UINT message,WPARAM w,LPARAM l){
    auto dialog=reinterpret_cast<Dialog*>(GetWindowLongPtrW(window,DWLP_USER));
    try{
        if(message==WM_INITDIALOG){
            dialog=reinterpret_cast<Dialog*>(l);SetWindowLongPtrW(window,DWLP_USER,l);
            const wchar_t* labels[]={L"v6 tiny — 速度优先",L"v6 small — 均衡",L"v6 medium — 较大模型",L"v5 mobile — 轻量"};
            for(auto label:labels)SendDlgItemMessageW(window,IDC_MODEL,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
            Selection current;try{current=ReadSelection(dialog->paths);dialog->originalChoice=current;dialog->originalValid=true;}catch(...){PluginLog("Invalid settings; model selection must be repaired");}
            int index=1;for(int i=0;i<4;++i)if(current.family==choices[i].family&&current.tier==choices[i].tier)index=i;
            SendDlgItemMessageW(window,IDC_MODEL,CB_SETCURSEL,index,0);
            CheckDlgButton(window,IDC_DIAGNOSTICS,current.diagnostics?BST_CHECKED:BST_UNCHECKED);return TRUE;
        }
        if(!dialog)return FALSE;
        if(message==WM_COMMAND&&LOWORD(w)==IDOK){
            auto selected=SendDlgItemMessageW(window,IDC_MODEL,CB_GETCURSEL,0,0);
            if(selected<0||selected>=4)return TRUE;
            dialog->choice=choices[selected];
            dialog->choice.diagnostics=IsDlgButtonChecked(window,IDC_DIAGNOSTICS)==BST_CHECKED;
            if(Unchanged(*dialog,dialog->choice)){
                if(dialog->choice.diagnostics!=dialog->originalChoice.diagnostics)SaveSelection(dialog->paths,dialog->choice);
                PluginLog("Settings unchanged; closing without model validation");EndDialog(window,0);return TRUE;
            }
            RunOperation(*dialog,window,false);
            if(dialog->needsDownload&&!dialog->cancel){
                auto prompt=L"所选模型 "+dialog->choice.family+L" "+dialog->choice.tier+
                    L" 尚未下载或文件不完整。是否现在下载？\n\n下载并校验成功后保存选择。选择“否”或取消下载将保留原模型并关闭设置。";
                if(MessageBoxW(window,prompt.c_str(),L"下载模型",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES){EndDialog(window,0);return TRUE;}
                RunOperation(*dialog,window,true);
            }
            if(dialog->success||dialog->cancel){EndDialog(window,0);return TRUE;}
            std::wstring status;{std::lock_guard<std::mutex> lock(dialog->progressMutex);status=dialog->status;}
            MessageBoxW(window,status.c_str(),L"模型设置失败",MB_OK|MB_ICONWARNING);return TRUE;
        }
        if(message==WM_CLOSE||(message==WM_COMMAND&&LOWORD(w)==IDCANCEL)){EndDialog(window,0);return TRUE;}
    }catch(const std::exception& e){PluginLog(e.what());MessageBoxW(window,L"设置操作失败，原模型保持不变。详情见 plugin.log。",L"模型设置失败",MB_OK|MB_ICONWARNING);EndDialog(window,1);return TRUE;}
    catch(...){EndDialog(window,1);return TRUE;}
    return FALSE;
}
}
int ShowSettings(const PluginPaths& paths,HINSTANCE instance){
    std::unique_lock<std::mutex> lock(settingsMutex,std::try_to_lock);if(!lock.owns_lock()){PluginLog("Settings already open; duplicate request rejected");return 1;}
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};if(!InitCommonControlsEx(&controls))return 1;
    Dialog dialog;dialog.paths=paths;dialog.instance=instance;
    PluginLog("Settings opened");
    HWND owner=GetForegroundWindow();DWORD ownerPid=0;
    if(owner)GetWindowThreadProcessId(owner,&ownerPid);
    if(ownerPid!=GetCurrentProcessId())owner=nullptr;
    auto result=DialogBoxParamW(instance,MAKEINTRESOURCEW(IDD_SETTINGS),owner,Procedure,reinterpret_cast<LPARAM>(&dialog));
    dialog.cancel=true;if(dialog.task.joinable())dialog.task.join();PluginLog("Settings closed result="+std::to_string(result));return result==0?0:1;
}

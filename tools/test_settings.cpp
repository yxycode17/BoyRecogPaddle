// 直接测试设置后台逻辑，不创建或自动操作 Windows 界面。
#include "../src/settings.cpp"
#include <cassert>
#include <iostream>
int wmain(int argc,wchar_t** argv){
    if(argc==1){
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);PluginPaths paths;
        paths.install=fs::path(exe).parent_path().parent_path().parent_path()/L"dist";
        paths.config=paths.install.parent_path()/L"build/settings-ui";fs::create_directories(paths.config);
        return ShowSettings(paths,GetModuleHandleW(nullptr));
    }
    assert(argc==2||argc==3);Dialog dialog;dialog.paths.install=fs::absolute(argv[1]);
    dialog.paths.config=dialog.paths.install.parent_path()/L"build/settings-tests"/std::to_wstring(GetCurrentProcessId());
    fs::create_directories(dialog.paths.config);
    auto initial=ReadSelection(dialog.paths);assert(initial.family==L"PP-OCRv6"&&initial.tier==L"small");
    dialog.originalChoice=initial;dialog.originalValid=true;
    assert(Unchanged(dialog,initial));assert(!Unchanged(dialog,Selection{L"PP-OCRv6",L"tiny"}));
    Dialog missing;missing.paths=dialog.paths;missing.choice={L"PP-OCRv6",L"tiny"};missing.save=true;
    RunTask(missing,false);assert(missing.done&&missing.needsDownload&&!missing.success);
    assert(ReadSelection(dialog.paths).tier==L"small");assert(!fs::exists(dialog.paths.config/L"settings.ini"));
    Dialog valid;valid.paths=dialog.paths;valid.choice=initial;valid.save=true;
    RunTask(valid,false);assert(valid.done&&valid.success&&!valid.needsDownload);
    valid.cancel=true;CommitSelection(valid);assert(!fs::exists(dialog.paths.config/L"settings.ini"));
    valid.cancel=false;CommitSelection(valid);assert(ReadSelection(dialog.paths).tier==L"small");
    auto diagnosticChoice=initial;diagnosticChoice.diagnostics=true;assert(Unchanged(dialog,diagnosticChoice));
    SaveSelection(dialog.paths,diagnosticChoice);assert(ReadSelection(dialog.paths).diagnostics);
    SaveSelection(dialog.paths,initial);assert(!ReadSelection(dialog.paths).diagnostics);
    if(argc==3){std::cout<<"Unchanged selection, missing-model prompt outcome and valid save checks passed"<<std::endl;return 0;}
    ResolveModel(dialog.paths,FindModel(initial));SaveSelection(dialog.paths,initial);
    dialog.choice={L"PP-OCRv6",L"tiny"};dialog.cancel=true;
    bool cancelled=false;try{Download(dialog);}catch(const std::exception&){cancelled=true;}assert(cancelled);
    assert(!fs::exists(DownloadRoot(dialog.paths,FindModel(dialog.choice))));assert(ReadSelection(dialog.paths).tier==L"small");
    dialog.cancel=false;
    std::atomic<bool> cancelFinished=false;
    std::thread canceller([&]{while(!cancelFinished){
        {std::lock_guard<std::mutex> guard(dialog.progressMutex);if(dialog.percent>0){dialog.cancel=true;return;}}
        Sleep(1);
    }});
    cancelled=false;try{Download(dialog);}catch(const std::exception&){cancelled=true;}
    cancelFinished=true;canceller.join();assert(cancelled&&dialog.cancel);
    assert(!fs::exists(DownloadRoot(dialog.paths,FindModel(dialog.choice))));assert(ReadSelection(dialog.paths).tier==L"small");
    dialog.cancel=false;
    std::atomic<bool> finished=false;
    std::thread reporter([&]{std::wstring previous;while(!finished){
        {std::lock_guard<std::mutex> guard(dialog.progressMutex);if(previous!=dialog.status){previous=dialog.status;std::cout<<"Download progress="<<dialog.percent<<"%"<<std::endl;}}
        Sleep(500);
    }});
    try{Download(dialog);}catch(...){finished=true;reporter.join();throw;}
    finished=true;reporter.join();
    auto root=DownloadRoot(dialog.paths,FindModel(dialog.choice));assert(ResolveModel(dialog.paths,FindModel(dialog.choice))==root);
    assert(ReadSelection(dialog.paths).tier==L"small");dialog.save=true;dialog.done=false;RunTask(dialog,true);
    assert(dialog.done&&dialog.success&&!dialog.needsDownload);CommitSelection(dialog);assert(ReadSelection(dialog.paths).tier==L"tiny");
    // 已完整下载时走本地验证，无需再次联网。
    Download(dialog);
    fs::rename(root/L"profile.json",root/L"profile.saved");bool rejected=false;
    try{ResolveModel(dialog.paths,FindModel(dialog.choice));}catch(const std::exception&){rejected=true;}assert(rejected);
    assert(ReadSelection(dialog.paths).tier==L"tiny");fs::rename(root/L"profile.saved",root/L"profile.json");
    std::ofstream(dialog.paths.config/L"test-result.txt")<<"PASS: HTTPS download, SHA256, mid-download cancellation, config persistence, missing-model rejection\n";
    std::cout<<"Download, SHA256, cancellation, config persistence and missing-model checks passed"<<std::endl;
}

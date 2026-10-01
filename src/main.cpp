#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <servprov.h>
#include <mmsystem.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <cstring>

// All Shell objects live on the UI STA. Only the fail-open watchdog uses a thread.
template<class T> struct Ptr {
 T* p=nullptr; ~Ptr(){if(p)p->Release();} T** out(){return &p;}
 T* operator->()const{return p;} operator T*()const{return p;}
 Ptr()=default; Ptr(const Ptr&)=delete; Ptr& operator=(const Ptr&)=delete;
};
void check(HRESULT h,const char* what){if(FAILED(h))throw std::runtime_error(what);}
std::atomic<bool> stopped{false}, blocked{false}, done{false};
std::atomic<unsigned> attempts{0};
bool keys[256]{}; HHOOK keyboard=nullptr,mouse=nullptr;
constexpr ULONG_PTR tag=0x5052414E;
DWORD lastAttempt=0; HWND targetWindow=nullptr;
struct Entry { std::wstring name, skipReason; BY_HANDLE_FILE_INFORMATION identity{}; bool checked=false; std::wstring path; std::vector<BYTE> child, recycled; POINT pos{}; bool eligible=false; bool moved=false; };
std::vector<Entry> entries; std::wstring journal; DWORD folderFlags=0;
IFolderView2* desktop=nullptr; size_t current=SIZE_MAX; bool dropReceived=false;
std::wstring failure;
std::atomic<unsigned long long> audioRequest{0}, audioApplied{0};
std::atomic<ULONGLONG> audioEnd{0};
std::atomic<bool> audioDone{false}, emergencyAudio{false}, audioFailed{false};
void requestAudio(int id){
 auto old=audioRequest.load();
 while(!audioRequest.compare_exchange_weak(old,(((old>>8)+1)<<8)|static_cast<unsigned>(id))){}
}
void stopNow(){stopped=true; blocked=false;}
void voice(int id,bool cancel=true){
 if(cancel&&stopped)return;
 if(id==16){bool expected=false;if(!emergencyAudio.compare_exchange_strong(expected,true))return;}
 requestAudio(id);
}
void intervention(){DWORD now=GetTickCount();if(!lastAttempt||now-lastAttempt>=6000){lastAttempt=now;attempts.fetch_add(1);}}
LRESULT CALLBACK keyHook(int n,WPARAM w,LPARAM l){
 if(n>=0){auto* k=reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
  if(k->dwExtraInfo==tag)return CallNextHookEx(keyboard,n,w,l);
  bool down=w==WM_KEYDOWN||w==WM_SYSKEYDOWN;
  if(k->vkCode<256)keys[k->vkCode]=down;
  bool ctrl=keys[VK_LCONTROL]||keys[VK_RCONTROL]||keys[VK_CONTROL];
  bool shift=keys[VK_LSHIFT]||keys[VK_RSHIFT]||keys[VK_SHIFT];
  if(ctrl&&shift&&keys['S']&&keys['P'])stopNow();
  if(blocked){if(down)intervention();return 1;}
 }return CallNextHookEx(keyboard,n,w,l);
}
LRESULT CALLBACK mouseHook(int n,WPARAM w,LPARAM l){
 if(n>=0&&blocked){auto* m=reinterpret_cast<MSLLHOOKSTRUCT*>(l);if(m->dwExtraInfo!=tag){intervention();return 1;}}
 return CallNextHookEx(mouse,n,w,l);
}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
bool waitMs(DWORD ms,bool cancellable=true){ULONGLONG until=GetTickCount64()+ms;do{pump();if(cancellable&&stopped)return false;Sleep(5);}while(GetTickCount64()<until);return true;}
DWORD wavDuration(const BYTE* p,DWORD size){
 if(size<12)return 0;
 DWORD rate=0,data=0;
 for(DWORD i=12;i+8<=size;){DWORD len;memcpy(&len,p+i+4,4);if(len>size-i-8)break;
 if(!memcmp(p+i,"fmt ",4)&&len>=16)memcpy(&rate,p+i+8+8,4);
 if(!memcmp(p+i,"data",4))data=len;
 if(len>MAXDWORD-i-9)break;
 i+=8+len+(len&1);}
 return rate?static_cast<DWORD>(1000ull*data/rate)+100:0;
}
void audioLoop(){
 unsigned observed=0;bool first=true;
 while(!audioDone){
  if(stopped)voice(16,false);
  else if(blocked){unsigned count=attempts.load();if(count!=observed){observed=count;voice(first?3:11);first=false;}}
  auto request=audioRequest.load();
  if(request!=audioApplied.load()){
   int id=static_cast<int>(request&255);HRSRC r=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
   const BYTE* p=r?static_cast<const BYTE*>(LockResource(LoadResource(nullptr,r))):nullptr;
   DWORD duration=p?wavDuration(p,SizeofResource(nullptr,r)):0;
   PlaySoundW(nullptr,nullptr,0);
   bool ok=p&&duration&&PlaySoundW(reinterpret_cast<LPCWSTR>(p),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
   audioEnd=GetTickCount64()+(ok?duration:0);audioApplied=request;
   if(!ok){audioFailed=true;stopNow();}
  }
  Sleep(5);
 }
 PlaySoundW(nullptr,nullptr,0);
}
void waitAudio(bool cancellable=true){
 do{if(!waitMs(5,cancellable))return;}while(audioApplied.load()!=audioRequest.load()||GetTickCount64()<audioEnd.load());
}

std::vector<BYTE> pidlBytes(PCUIDLIST_RELATIVE p){UINT n=ILGetSize(p);return std::vector<BYTE>(reinterpret_cast<const BYTE*>(p),reinterpret_cast<const BYTE*>(p)+n);}
PCIDLIST_ABSOLUTE absPidl(const std::vector<BYTE>& v){return reinterpret_cast<PCIDLIST_ABSOLUTE>(v.data());}
PCUITEMID_CHILD childPidl(const std::vector<BYTE>& v){return reinterpret_cast<PCUITEMID_CHILD>(v.data());}
void writeBytes(HANDLE h,const void* p,DWORD n){DWORD wrote=0;if(!WriteFile(h,p,n,&wrote,nullptr)||wrote!=n)throw std::runtime_error("Journal write failed");}
void save(){
 std::wstring tmp=journal+L".tmp";HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create recovery journal");
 try{DWORD magic=0x32524A50,count=static_cast<DWORD>(entries.size());writeBytes(h,&magic,4);writeBytes(h,&folderFlags,4);writeBytes(h,&count,4);
 for(auto& e:entries){DWORD n=static_cast<DWORD>(e.path.size());writeBytes(h,&n,4);writeBytes(h,e.path.data(),n*2);writeBytes(h,&e.pos,sizeof(e.pos));
 DWORD state=(e.eligible?1:0)|(e.moved?2:0);writeBytes(h,&state,4);
 for(auto* v:{&e.child,&e.recycled}){n=static_cast<DWORD>(v->size());writeBytes(h,&n,4);writeBytes(h,v->data(),n);}}
 if(!FlushFileBuffers(h))throw std::runtime_error("Cannot flush recovery journal");
 CloseHandle(h);h=INVALID_HANDLE_VALUE;
 if(!MoveFileExW(tmp.c_str(),journal.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot commit recovery journal");
 }catch(...){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);throw;}
}
void readBytes(HANDLE h,void* p,DWORD n){DWORD got;if(!ReadFile(h,p,n,&got,nullptr)||got!=n)throw std::runtime_error("Invalid recovery journal");}
void load(){HANDLE h=CreateFileW(journal.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open recovery journal");
 try{DWORD magic,count;readBytes(h,&magic,4);if(magic!=0x32524A50)throw std::runtime_error("Invalid journal version");readBytes(h,&folderFlags,4);readBytes(h,&count,4);if(count>10000)throw std::runtime_error("Invalid entry count");
 entries.clear();for(DWORD i=0;i<count;i++){Entry e;DWORD n,state;readBytes(h,&n,4);if(n>32768)throw std::runtime_error("Invalid path");e.path.resize(n);readBytes(h,e.path.data(),n*2);readBytes(h,&e.pos,sizeof(e.pos));readBytes(h,&state,4);e.eligible=state&1;e.moved=state&2;
 for(auto* v:{&e.child,&e.recycled}){readBytes(h,&n,4);if(n>65536||(n&&n<2))throw std::runtime_error("Invalid PIDL");v->resize(n);readBytes(h,v->data(),n);if(n){size_t offset=0;while(offset+2<=n){USHORT len;memcpy(&len,v->data()+offset,2);if(!len){if(offset+2!=n)throw std::runtime_error("Malformed PIDL");break;}if(len<2||offset+len>n-2)throw std::runtime_error("Malformed PIDL");offset+=len;}if(offset+2!=n)throw std::runtime_error("Malformed PIDL");}}
 if(e.child.empty()||(e.moved&&e.recycled.empty()))throw std::runtime_error("Incomplete journal entry");
 entries.push_back(std::move(e));}CloseHandle(h);
 }catch(...){CloseHandle(h);throw;}}
void getDesktop(){
 Ptr<IShellWindows> windows;check(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(windows.out())),"Cannot access Explorer");
 VARIANT empty;VariantInit(&empty);long hwnd=0;Ptr<IDispatch> dispatch;
 check(windows->FindWindowSW(&empty,&empty,SWC_DESKTOP,&hwnd,SWFO_NEEDDISPATCH,dispatch.out()),"Desktop not found");
 Ptr<IServiceProvider> service;check(dispatch->QueryInterface(IID_PPV_ARGS(service.out())),"Desktop service unavailable");
 Ptr<IShellBrowser> browser;check(service->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(browser.out())),"Desktop browser unavailable");
 Ptr<IShellView> view;check(browser->QueryActiveShellView(view.out()),"Desktop view unavailable");
 check(view->QueryInterface(IID_PPV_ARGS(&desktop)),"Desktop folder view unavailable");
}
std::wstring display(IShellItem* item,SIGDN kind){PWSTR p=nullptr;if(FAILED(item->GetDisplayName(kind,&p)))return {};std::wstring s=p;CoTaskMemFree(p);return s;}
POINT screenPoint(POINT p){Ptr<IOleWindow> window;check(desktop->QueryInterface(IID_PPV_ARGS(window.out())),"View window unavailable");HWND h;check(window->GetWindow(&h),"View handle unavailable");ClientToScreen(h,&p);return p;}
POINT iconCenter(const Entry& e){
 POINT position{};check(desktop->GetItemPosition(childPidl(e.child),&position),"Cannot locate current icon");
 POINT spacing{};FOLDERVIEWMODE mode=FVM_ICON;int size=32;
 check(desktop->GetSpacing(&spacing),"Cannot read icon spacing");check(desktop->GetViewModeAndIconSize(&mode,&size),"Cannot read icon size");
 position.x+=spacing.x/2;position.y+=size/2+4;return screenPoint(position);
}
bool probeTree(const std::wstring& path,std::wstring& reason,unsigned depth=0){
 if(depth>128){reason=L"Слишком большая глубина папок: "+path;return false;}
 DWORD attrs=GetFileAttributesW(path.c_str());
 if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_READONLY))){reason=L"Недоступный/защищённый вложенный элемент: "+path;return false;}
 DWORD access=DELETE|FILE_READ_ATTRIBUTES;if(attrs&FILE_ATTRIBUTE_DIRECTORY)access|=FILE_LIST_DIRECTORY;
 HANDLE h=CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(h==INVALID_HANDLE_VALUE){reason=L"Нет доступа или вложенный элемент занят: "+path;return false;}CloseHandle(h);
 if(!(attrs&FILE_ATTRIBUTE_DIRECTORY))return true;
 WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((path+L"\\*").c_str(),&data);
 if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return true;reason=L"Нельзя прочитать папку: "+path;return false;}
 bool ok=true;DWORD error=ERROR_NO_MORE_FILES;
 do{if(wcscmp(data.cFileName,L".")&&wcscmp(data.cFileName,L"..")&&!probeTree(path+L"\\"+data.cFileName,reason,depth+1)){ok=false;break;}}while(FindNextFileW(find,&data));
 if(ok)error=GetLastError();
 FindClose(find);
 if(ok&&error!=ERROR_NO_MORE_FILES){reason=L"Ошибка перечисления папки: "+path;return false;}return ok;
}
bool probe(Entry& e,bool record){
 auto reject=[&](const wchar_t* reason){e.skipReason=reason;return false;};
 DWORD attrs=GetFileAttributesW(e.path.c_str());
 if(attrs==INVALID_FILE_ATTRIBUTES)return reject(L"Элемент исчез или атрибуты недоступны");
 if(attrs&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_READONLY))return reject(L"Системный, readonly или reparse-элемент");
 Ptr<IShellItem> item;if(FAILED(SHCreateItemFromParsingName(e.path.c_str(),nullptr,IID_PPV_ARGS(item.out()))))return reject(L"Shell не может открыть элемент");
 SFGAOF capabilities=0;if(FAILED(item->GetAttributes(SFGAO_CANDELETE|SFGAO_CANMOVE,&capabilities))||(capabilities&(SFGAO_CANDELETE|SFGAO_CANMOVE))!=(SFGAO_CANDELETE|SFGAO_CANMOVE))return reject(L"Shell не разрешает перемещение/удаление");
 // Opening with DELETE probes permissions and sharing restrictions; nothing is deleted.
 HANDLE source=CreateFileW(e.path.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(source==INVALID_HANDLE_VALUE)return reject(L"Нет DELETE-доступа или элемент занят без FILE_SHARE_DELETE");
 BY_HANDLE_FILE_INFORMATION info{};bool got=GetFileInformationByHandle(source,&info)!=FALSE;CloseHandle(source);
 if(!got)return reject(L"Не удалось проверить идентификатор элемента");
 if(!record&&e.checked&&(info.dwVolumeSerialNumber!=e.identity.dwVolumeSerialNumber||info.nFileIndexHigh!=e.identity.nFileIndexHigh||info.nFileIndexLow!=e.identity.nFileIndexLow))return reject(L"По исходному пути теперь другой объект");
 std::wstring parent=e.path.substr(0,e.path.find_last_of(L"\\/"));
 DWORD access=(attrs&FILE_ATTRIBUTE_DIRECTORY)?FILE_ADD_SUBDIRECTORY:FILE_ADD_FILE;
 HANDLE destination=CreateFileW(parent.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(destination==INVALID_HANDLE_VALUE)return reject(L"Нет доступа для восстановления в исходную папку");
 CloseHandle(destination);
 if((attrs&FILE_ATTRIBUTE_DIRECTORY)&&!probeTree(e.path,e.skipReason))return false;
 if(record){e.identity=info;e.checked=true;}e.skipReason.clear();return true;
}
void writeReport(const std::wstring& suffix,const std::wstring& contents){
 std::wstring path=journal.empty()?L"":journal+suffix;if(path.empty())return;
 int length=WideCharToMultiByte(CP_UTF8,0,contents.data(),static_cast<int>(contents.size()),nullptr,0,nullptr,nullptr);
 std::string bytes(length,'\0');WideCharToMultiByte(CP_UTF8,0,contents.data(),static_cast<int>(contents.size()),bytes.data(),length,nullptr,nullptr);
 HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(h==INVALID_HANDLE_VALUE)return;
 DWORD wrote=0;WriteFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&wrote,nullptr);FlushFileBuffers(h);CloseHandle(h);
}
void writePlan(){
 std::wstring contents=L"status\tname\tpath\treason\n";
 for(auto& e:entries)contents+=(e.eligible?L"READY":L"SKIP")+std::wstring(L"\t")+e.name+L"\t"+e.path+L"\t"+e.skipReason+L"\n";
 writeReport(L".plan.tsv",contents);
}
void snapshot(){
 check(desktop->GetCurrentFolderFlags(&folderFlags),"Cannot read icon settings");
 Ptr<IShellFolder> folder;check(desktop->GetFolder(IID_PPV_ARGS(folder.out())),"Cannot access desktop folder");
 PWSTR publicPath=nullptr;check(SHGetKnownFolderPath(FOLDERID_PublicDesktop,0,nullptr,&publicPath),"Public desktop path unavailable");std::wstring publicRoot=publicPath;CoTaskMemFree(publicPath);
 PWSTR root=nullptr;check(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&root),"Desktop path unavailable");std::wstring userRoot=root;CoTaskMemFree(root);
 int count=0;check(desktop->ItemCount(SVGIO_ALLVIEW,&count),"Cannot enumerate icons");
 for(int i=0;i<count;i++){PITEMID_CHILD child=nullptr;check(desktop->Item(i,&child),"Cannot get icon");Entry e;e.child=pidlBytes(child);check(desktop->GetItemPosition(child,&e.pos),"Cannot read icon position");
 Ptr<IShellItem> item;HRESULT hr=SHCreateItemWithParent(nullptr,folder,child,IID_PPV_ARGS(item.out()));CoTaskMemFree(child);check(hr,"Cannot resolve desktop item");e.path=display(item,SIGDN_FILESYSPATH);e.name=display(item,SIGDN_NORMALDISPLAY);e.skipReason=L"Виртуальный или системный значок";
 if(!e.path.empty()){size_t slash=e.path.find_last_of(L"\\/");std::wstring parent=e.path.substr(0,slash);DWORD attrs=GetFileAttributesW(e.path.c_str());
 wchar_t volume[MAX_PATH]{};bool local=GetVolumePathNameW(e.path.c_str(),volume,MAX_PATH)&&GetDriveTypeW(volume)==DRIVE_FIXED;
 bool onDesktop=_wcsicmp(parent.c_str(),userRoot.c_str())==0||_wcsicmp(parent.c_str(),publicRoot.c_str())==0;
 e.eligible=local&&onDesktop&&attrs!=INVALID_FILE_ATTRIBUTES;
 if(e.eligible)e.eligible=probe(e,true);else e.skipReason=L"Недоступный путь или не локальный рабочий стол";}
 entries.push_back(std::move(e));}
 PWSTR local=nullptr;check(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local),"LocalAppData unavailable");std::wstring dir=std::wstring(local)+L"\\DesktopPrank";CoTaskMemFree(local);CreateDirectoryW(dir.c_str(),nullptr);
 SYSTEMTIME t;GetLocalTime(&t);wchar_t name[96];swprintf(name,96,L"\\recovery-%04u%02u%02u-%02u%02u%02u-%lu.prj",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId());journal=dir+name;save();writePlan();
}
struct Sink : IFileOperationProgressSink {
 ULONG refs=1; Entry* entry; HRESULT result=E_FAIL; bool captured=false;
 explicit Sink(Entry* e):entry(e){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p)override{if(id==IID_IUnknown||id==IID_IFileOperationProgressSink){*p=this;AddRef();return S_OK;}*p=nullptr;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{return --refs;}
 HRESULT STDMETHODCALLTYPE StartOperations()override{return S_OK;}HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD,IShellItem*,LPCWSTR)override{return E_ABORT;}
 HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD flags,IShellItem*,IShellItem*,LPCWSTR)override{
 if((flags&TSF_OVERWRITE_EXIST)||GetFileAttributesW(entry->path.c_str())!=INVALID_FILE_ATTRIBUTES)return E_ABORT;
 return S_OK;
 }
 HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT h,IShellItem*)override{result=h;return S_OK;}
 HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR)override{return E_ABORT;}
 HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags,IShellItem*)override{
 // Fail closed if Shell proposes permanent deletion, even if the bin is disabled/full.
 return !stopped&&(flags&TSF_DELETE_RECYCLE_IF_POSSIBLE)?S_OK:E_ABORT;
 }
 HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,IShellItem*,HRESULT h,IShellItem* recycled)override{
 result=h;if(SUCCEEDED(h)&&recycled){PIDLIST_ABSOLUTE p=nullptr;h=SHGetIDListFromObject(recycled,&p);if(SUCCEEDED(h)){entry->recycled=pidlBytes(p);CoTaskMemFree(p);entry->moved=true;captured=true;try{save();}catch(...){failure=L"Не удалось записать результат переноса. Восстановление будет выполнено сейчас.";stopNow();return E_FAIL;}}}return S_OK;
 }
 HRESULT STDMETHODCALLTYPE PreNewItem(DWORD,IShellItem*,LPCWSTR)override{return E_ABORT;}
 HRESULT STDMETHODCALLTYPE PostNewItem(DWORD,IShellItem*,LPCWSTR,LPCWSTR,DWORD,HRESULT,IShellItem*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE UpdateProgress(UINT,UINT)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE ResetTimer()override{return S_OK;}HRESULT STDMETHODCALLTYPE PauseTimer()override{return S_OK;}HRESULT STDMETHODCALLTYPE ResumeTimer()override{return S_OK;}
};
void recycle(Entry& e){
 if(stopped)return;
 if(!probe(e,false)){e.eligible=false;save();writePlan();return;}
 save();Ptr<IShellItem> item;check(SHCreateItemFromParsingName(e.path.c_str(),nullptr,IID_PPV_ARGS(item.out())),"Item disappeared before drag");
 Ptr<IFileOperation> op;check(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(op.out())),"File operation unavailable");
 check(op->SetOperationFlags(FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_NOERRORUI|FOF_SILENT|FOF_NO_CONNECTED_ELEMENTS|FOFX_RECYCLEONDELETE|FOFX_EARLYFAILURE),"Cannot set recycle flags");
 Sink sink(&e);check(op->DeleteItem(item,&sink),"Cannot schedule recycle");HRESULT h=op->PerformOperations();BOOL aborted=FALSE;op->GetAnyOperationsAborted(&aborted);
 if(FAILED(h)||aborted||FAILED(sink.result)||!sink.captured)throw std::runtime_error("Recycle failed or no recovery identity was returned");
}
struct DropTarget : IDropTarget {
 ULONG refs=1;bool acceptable=false;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p)override{if(id==IID_IUnknown||id==IID_IDropTarget){*p=this;AddRef();return S_OK;}*p=nullptr;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{return --refs;}
 bool matches(IDataObject* data){if(current>=entries.size()||stopped)return false;Ptr<IShellItemArray> array;if(FAILED(SHCreateShellItemArrayFromDataObject(data,IID_PPV_ARGS(array.out()))))return false;DWORD count=0;array->GetCount(&count);if(count!=1)return false;Ptr<IShellItem> item;if(FAILED(array->GetItemAt(0,item.out())))return false;return _wcsicmp(display(item,SIGDN_FILESYSPATH).c_str(),entries[current].path.c_str())==0;}
 HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD,POINTL,DWORD* effect)override{acceptable=matches(data);*effect=acceptable?DROPEFFECT_MOVE:DROPEFFECT_NONE;return S_OK;}
 HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD* effect)override{*effect=acceptable&&!stopped?DROPEFFECT_MOVE:DROPEFFECT_NONE;return S_OK;}
 HRESULT STDMETHODCALLTYPE DragLeave()override{acceptable=false;return S_OK;}
 HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,DWORD,POINTL,DWORD* effect)override{
 *effect=DROPEFFECT_NONE;dropReceived=true;if(!matches(data))return E_ABORT;
 try{recycle(entries[current]);
 // The target has already moved the item. Never ask Explorer to delete the source again.
 FORMATETC format{};format.cfFormat=static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));format.dwAspect=DVASPECT_CONTENT;format.lindex=-1;format.tymed=TYMED_HGLOBAL;
 STGMEDIUM medium{};medium.tymed=TYMED_HGLOBAL;medium.hGlobal=GlobalAlloc(GMEM_MOVEABLE,sizeof(DWORD));
 if(medium.hGlobal){auto* value=static_cast<DWORD*>(GlobalLock(medium.hGlobal));if(value){*value=DROPEFFECT_NONE;GlobalUnlock(medium.hGlobal);if(FAILED(data->SetData(&format,&medium,TRUE)))ReleaseStgMedium(&medium);}else GlobalFree(medium.hGlobal);}
 *effect=DROPEFFECT_NONE;
 }catch(...){auto& entry=entries[current];
 if(!entry.moved&&GetFileAttributesW(entry.path.c_str())!=INVALID_FILE_ATTRIBUTES){entry.eligible=false;entry.skipReason=L"Shell отклонила перенос; пропущен";try{save();writePlan();}catch(...){stopNow();}return E_FAIL;}
 failure=L"Перенос остановлен: Windows не подтвердила безопасный перенос в Корзину.";stopNow();return E_FAIL;}return S_OK;
 }
};
LRESULT CALLBACK windowProc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;return DefWindowProcW(h,m,w,l);}
void mouseInput(DWORD flags,POINT p){INPUT in{};in.type=INPUT_MOUSE;in.mi.dwFlags=flags;in.mi.dwExtraInfo=tag;
 if(flags&MOUSEEVENTF_MOVE){int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN),width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);in.mi.dx=MulDiv(p.x-x,65535,std::max(1,width-1));in.mi.dy=MulDiv(p.y-y,65535,std::max(1,height-1));in.mi.dwFlags|=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;}
 if(SendInput(1,&in,sizeof(in))!=1)throw std::runtime_error("Mouse injection failed");
}
void glide(POINT from,POINT to){double distance=std::hypot(double(to.x-from.x),double(to.y-from.y));int steps=std::max(1,int(std::ceil(distance/13.5)));
 for(int i=1;i<=steps&&!stopped;i++){POINT p{from.x+LONG(std::lround(double(to.x-from.x)*i/steps)),from.y+LONG(std::lround(double(to.y-from.y)*i/steps))};mouseInput(MOUSEEVENTF_MOVE,p);waitMs(25);}}
struct DragRelease {
 bool pressed=false;
 ~DragRelease(){if(!pressed)return;INPUT in[3]{};for(auto& x:in)x.type=INPUT_KEYBOARD;
 in[0].ki.wVk=VK_ESCAPE;in[0].ki.dwExtraInfo=tag;in[1].ki.wVk=VK_ESCAPE;in[1].ki.dwFlags=KEYEVENTF_KEYUP;in[1].ki.dwExtraInfo=tag;
 in[2].type=INPUT_MOUSE;in[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;in[2].mi.dwExtraInfo=tag;SendInput(3,in,sizeof(INPUT));}
};
void drag(size_t index,POINT bin){
 if(!probe(entries[index],false)){entries[index].eligible=false;save();writePlan();return;}
 current=index;dropReceived=false;POINT start=iconCenter(entries[index]);
 auto selected=childPidl(entries[index].child);
 check(desktop->SelectAndPositionItems(1,&selected,nullptr,SVSI_SELECT|SVSI_DESELECTOTHERS),"Cannot select source icon");
 POINT cursor;GetCursorPos(&cursor);glide(cursor,start);if(stopped)return;
 DragRelease release;mouseInput(MOUSEEVENTF_LEFTDOWN,start);release.pressed=true;waitMs(80);glide(start,bin);
 if(stopped){INPUT esc{};esc.type=INPUT_KEYBOARD;esc.ki.wVk=VK_ESCAPE;esc.ki.dwExtraInfo=tag;SendInput(1,&esc,sizeof(esc));esc.ki.dwFlags=KEYEVENTF_KEYUP;SendInput(1,&esc,sizeof(esc));}
 mouseInput(MOUSEEVENTF_LEFTUP,bin);release.pressed=false;waitMs(500);
 if(!stopped&&!dropReceived)throw std::runtime_error("Explorer did not deliver the mouse drop");
 current=SIZE_MAX;
}
std::wstring restore(){
 blocked=false;if(targetWindow)ShowWindow(targetWindow,SW_HIDE);std::wstring errors;
 for(auto& e:entries){if(!e.moved)continue;
 if(GetFileAttributesW(e.path.c_str())!=INVALID_FILE_ATTRIBUTES){errors+=L"Конфликт имени: "+e.path+L"\n";continue;}
 try{size_t split=e.path.find_last_of(L"\\/");if(split==std::wstring::npos)throw std::runtime_error("Invalid destination");Ptr<IShellItem> source,parent;
 check(SHCreateItemFromIDList(absPidl(e.recycled),IID_PPV_ARGS(source.out())),"Recycled item unavailable");check(SHCreateItemFromParsingName(e.path.substr(0,split).c_str(),nullptr,IID_PPV_ARGS(parent.out())),"Original folder unavailable");
 Ptr<IFileOperation> op;check(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(op.out())),"Restore operation unavailable");
 // PreMoveItem refuses overwrite, including conflicts created after the initial check.
 check(op->SetOperationFlags(FOF_NOERRORUI|FOF_SILENT|FOF_NOCONFIRMATION|FOF_NO_CONNECTED_ELEMENTS|FOFX_EARLYFAILURE),"Cannot set restore flags");Sink sink(&e);check(op->MoveItem(source,parent,e.path.substr(split+1).c_str(),&sink),"Cannot schedule restore");check(op->PerformOperations(),"Restore failed");BOOL aborted=FALSE;op->GetAnyOperationsAborted(&aborted);if(aborted||FAILED(sink.result)||GetFileAttributesW(e.path.c_str())==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Restore not confirmed");e.moved=false;save();
 }catch(...){errors+=L"Не восстановлен: "+e.path+L"\n";}}
 waitMs(1500,false);
 HRESULT h=desktop->SetCurrentFolderFlags(FWF_AUTOARRANGE|FWF_SNAPTOGRID,0);if(FAILED(h))errors+=L"Не удалось отключить выравнивание значков.\n";
 for(auto& e:entries){if(e.moved)continue;auto child=childPidl(e.child);POINT p=e.pos;h=desktop->SelectAndPositionItems(1,&child,&p,SVSI_POSITIONITEM);if(FAILED(h))errors+=L"Не удалось вернуть позицию: "+e.path+L"\n";}
 h=desktop->SetCurrentFolderFlags(FWF_AUTOARRANGE|FWF_SNAPTOGRID,folderFlags&(FWF_AUTOARRANGE|FWF_SNAPTOGRID));if(FAILED(h))errors+=L"Не удалось вернуть настройки значков.\n";
 waitMs(500,false);
 for(auto& e:entries){if(e.moved)continue;POINT p{};if(FAILED(desktop->GetItemPosition(childPidl(e.child),&p))||p.x!=e.pos.x||p.y!=e.pos.y){errors+=L"Позиция отличается после восстановления: "+e.path+L"\n";}}
 return errors;
}

POINT recyclePoint(){PIDLIST_ABSOLUTE recycleId=nullptr;check(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder,0,nullptr,&recycleId),"Recycle Bin unavailable");
 POINT p{};bool found=false;for(auto& e:entries){Ptr<IShellItem> item;Ptr<IShellItem> bin;Ptr<IShellFolder> folder;desktop->GetFolder(IID_PPV_ARGS(folder.out()));if(SUCCEEDED(SHCreateItemWithParent(nullptr,folder,childPidl(e.child),IID_PPV_ARGS(item.out())))&&SUCCEEDED(SHCreateItemFromIDList(recycleId,IID_PPV_ARGS(bin.out())))){int order=1;if(SUCCEEDED(item->Compare(bin,SICHINT_CANONICAL,&order))&&order==0){p=iconCenter(e);found=true;break;}}}CoTaskMemFree(recycleId);if(!found)throw std::runtime_error("Recycle Bin icon must be visible on desktop");return p;}
BOOL CALLBACK minimizeWindow(HWND h,LPARAM){
 if(!IsWindowVisible(h)||h==targetWindow||h==GetShellWindow()||GetWindow(h,GW_OWNER))return TRUE;
 wchar_t cls[128]{};GetClassNameW(h,cls,128);
 if(wcscmp(cls,L"Shell_TrayWnd")==0||wcscmp(cls,L"Shell_SecondaryTrayWnd")==0||wcscmp(cls,L"WorkerW")==0||wcscmp(cls,L"Progman")==0)return TRUE;
 if(GetWindowLongPtrW(h,GWL_STYLE)&WS_CAPTION)ShowWindowAsync(h,SW_FORCEMINIMIZE);
 return TRUE;
}
void showDesktop(){EnumWindows(minimizeWindow,0);waitMs(700);}
int selfTest(){
 if(FAILED(OleInitialize(nullptr)))return 1;
 struct ComCleanup{~ComCleanup(){OleUninitialize();}} comCleanup;
 try{
 for(int id:{1,2,3,4,5,6,7,8,9,10,11,13,15,16}){HRSRC r=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);if(!r)throw std::runtime_error("Missing audio");auto* p=static_cast<const BYTE*>(LockResource(LoadResource(nullptr,r)));DWORD size=SizeofResource(nullptr,r);if(size<12||memcmp(p,"RIFF",4)||memcmp(p+8,"WAVE",4)||wavDuration(p,size)<100)throw std::runtime_error("Invalid audio");}
 wchar_t dir[MAX_PATH]{},name[MAX_PATH]{};if(!GetTempPathW(MAX_PATH,dir)||!GetTempFileNameW(dir,L"PRJ",0,name))throw std::runtime_error("Temp unavailable");journal=name;
 Entry e;e.path=L"C:\\Тест\\Новая папка";e.child={0,0};e.recycled={0,0};e.pos={-123,987};e.eligible=true;e.moved=true;entries={e};folderFlags=FWF_AUTOARRANGE|FWF_SNAPTOGRID;save();entries.clear();load();
 bool match=entries.size()==1&&entries[0].path==e.path&&entries[0].pos.x==e.pos.x&&entries[0].pos.y==e.pos.y&&entries[0].moved&&entries[0].recycled==e.recycled&&folderFlags==(FWF_AUTOARRANGE|FWF_SNAPTOGRID);
 HANDLE h=CreateFileW(journal.c_str(),GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,0,nullptr);if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);bool rejected=false;try{load();}catch(...){rejected=true;}DeleteFileW(journal.c_str());DeleteFileW((journal+L".tmp").c_str());if(!match||!rejected)throw std::runtime_error("Journal roundtrip failed");
 if(!GetTempFileNameW(dir,L"PRF",0,name))throw std::runtime_error("Probe temp unavailable");
 struct TempCleanup{std::wstring path;~TempCleanup(){SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);DeleteFileW(path.c_str());}} cleanup{name};
 Entry available;available.path=name;if(!probe(available,true))throw std::runtime_error("Available file rejected");
 HANDLE locked=CreateFileW(name,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(locked==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot lock test file");bool denied=!probe(available,false);CloseHandle(locked);if(!denied)throw std::runtime_error("Locked file accepted");
 if(!SetFileAttributesW(name,FILE_ATTRIBUTE_READONLY))throw std::runtime_error("Cannot set readonly");
 if(probe(available,false))throw std::runtime_error("Readonly file accepted");
 SetFileAttributesW(name,FILE_ATTRIBUTE_NORMAL);
 available.identity.nFileIndexLow^=1;if(probe(available,false))throw std::runtime_error("Changed identity accepted");
 return 0;
 }catch(...){return 1;}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
 int testArgc=0;LPWSTR* testArgv=CommandLineToArgvW(GetCommandLineW(),&testArgc);bool testing=testArgv&&testArgc==2&&std::wstring(testArgv[1])==L"--self-test";if(testArgv)LocalFree(testArgv);if(testing)return selfTest();
 BOOL(WINAPI* dpi)(HANDLE)=nullptr;auto proc=GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext");static_assert(sizeof(dpi)==sizeof(proc));memcpy(&dpi,&proc,sizeof(dpi));if(dpi)dpi(reinterpret_cast<HANDLE>(-4));
 if(FAILED(OleInitialize(nullptr)))return 1;
 HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\DesktopPrank.OneInstance");if(!mutex||GetLastError()==ERROR_ALREADY_EXISTS){if(mutex)CloseHandle(mutex);OleUninitialize();return 1;}
 std::thread audio(audioLoop);
 std::thread watchdog;DropTarget target;bool registered=false;int status=0;
 try{int argc;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool recovery=argc>=2&&std::wstring(argv[1])==L"--restore";
 if(recovery){
 if(argc>=3)journal=argv[2];else{
  PWSTR local=nullptr;check(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local),"Recovery directory unavailable");std::wstring dir=std::wstring(local)+L"\\DesktopPrank\\";CoTaskMemFree(local);
  WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW((dir+L"recovery-*.prj").c_str(),&data);FILETIME newest{};
  if(find!=INVALID_HANDLE_VALUE){do{if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&CompareFileTime(&data.ftLastWriteTime,&newest)>0){newest=data.ftLastWriteTime;journal=dir+data.cFileName;}}while(FindNextFileW(find,&data));FindClose(find);}
 }
 LocalFree(argv);if(journal.empty())throw std::runtime_error("No recovery journal found");getDesktop();load();voice(16,false);std::wstring errors=restore();waitAudio(false);writeReport(L".result.txt",errors.empty()?L"Восстановление завершено.":errors);
 }
 else{LocalFree(argv);
 getDesktop();snapshot();POINT bin=recyclePoint();
 WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=windowProc;cls.lpszClassName=L"DesktopPrank.Drop";RegisterClassW(&cls);
 targetWindow=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,cls.lpszClassName,L"",WS_POPUP,bin.x-24,bin.y-24,64,64,nullptr,nullptr,instance,nullptr);if(!targetWindow)throw std::runtime_error("Cannot create drop target");SetLayeredWindowAttributes(targetWindow,0,1,LWA_ALPHA);check(RegisterDragDrop(targetWindow,&target),"Cannot register mouse drop target");registered=true;
 HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ready)throw std::runtime_error("Cannot create hook event");
 watchdog=std::thread([instance,ready]{
 keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,keyHook,instance,0);mouse=SetWindowsHookExW(WH_MOUSE_LL,mouseHook,instance,0);SetEvent(ready);
 ULONGLONG start=0;
 while(!done){pump();if(blocked){if(!start)start=GetTickCount64();if(GetTickCount64()-start>180000){stopNow();}}Sleep(5);}
 if(keyboard)UnhookWindowsHookEx(keyboard);
 if(mouse)UnhookWindowsHookEx(mouse);
 });
 DWORD readyResult=WaitForSingleObject(ready,5000);CloseHandle(ready);if(readyResult!=WAIT_OBJECT_0||!keyboard||!mouse)throw std::runtime_error("Cannot install input hooks");
 check(desktop->SetCurrentFolderFlags(FWF_AUTOARRANGE|FWF_SNAPTOGRID,0),"Cannot freeze icon positions");
 showDesktop();ShowWindow(targetWindow,SW_SHOWNOACTIVATE);blocked=true;
 
 voice(1);waitAudio();voice(2);waitAudio();size_t total=std::count_if(entries.begin(),entries.end(),[](const Entry& e){return e.eligible;});if(total>=15)voice(10);
 unsigned moved=0;
 for(size_t i=0;i<entries.size()&&!stopped;i++){auto& e=entries[i];if(!e.eligible)continue;DWORD attrs=GetFileAttributesW(e.path.c_str());bool folder=attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY);
 std::wstring name=e.path.substr(e.path.find_last_of(L"\\/")+1);if(folder&&name==L"Новая папка"){voice(6);waitAudio();}
 if(folder)voice(7);
 if(moved==3){voice(9);waitMs(1100);}if(stopped)break;
 drag(i,bin);if(e.moved){++moved;if(moved==1)voice(4);if(moved>=2)voice(5);if(moved==3)voice(8);}}
 blocked=false;ShowWindow(targetWindow,SW_HIDE);
 if(!stopped){check(desktop->SetCurrentFolderFlags(FWF_AUTOARRANGE|FWF_SNAPTOGRID,folderFlags&(FWF_AUTOARRANGE|FWF_SNAPTOGRID)),"Cannot restore desktop settings");voice(13);waitAudio();voice(15);waitAudio();}
 if(stopped){voice(16,false);auto errors=restore();waitAudio(false);writeReport(L".result.txt",errors.empty()?L"Восстановление завершено.":errors);}
 else{std::wstring message=L"Уборка завершена. Управление возвращено.\nВосстановление: запустите EXE с --restore с путём журнала:\n"+journal;writeReport(L".result.txt",message);}
 }}catch(const std::exception& ex){stopNow();if(targetWindow)ShowWindow(targetWindow,SW_HIDE);
 if(std::string(ex.what())!="Cancelled"){std::wstring text=L"Работа остановлена; ввод разблокирован.\n";std::string narrow=ex.what();text.append(narrow.begin(),narrow.end());text+=L"\n"+failure;
 if(desktop&&!entries.empty()){try{voice(16,false);text+=L"\n"+restore();}catch(...){text+=L"\nАвтовосстановление не завершено. Используйте --restore.";}}if(!journal.empty())text+=L"\nЖурнал: "+journal;waitAudio(false);writeReport(L".result.txt",text);status=1;}}
 blocked=false;done=true;if(watchdog.joinable())watchdog.join();audioDone=true;audio.join();if(registered)RevokeDragDrop(targetWindow);if(targetWindow)DestroyWindow(targetWindow);if(desktop)desktop->Release();ReleaseMutex(mutex);CloseHandle(mutex);OleUninitialize();return status;
}

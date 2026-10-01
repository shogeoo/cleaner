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

// Shell calls run on the UI STA; audio and input hooks use separate threads.
template<class T> struct Ptr {
 T* p=nullptr; ~Ptr(){if(p)p->Release();} T** out(){return &p;}
 T* operator->()const{return p;} operator T*()const{return p;}
 Ptr()=default; Ptr(const Ptr&)=delete; Ptr& operator=(const Ptr&)=delete;
};
void check(HRESULT h,const char* what){if(FAILED(h))throw std::runtime_error(what);}
std::atomic<bool> blocked{false}, done{false};
std::atomic<unsigned> attempts{0};
HHOOK keyboard=nullptr,mouse=nullptr;
constexpr ULONG_PTR tag=0x5052414E;
DWORD lastAttempt=0; HWND targetWindow=nullptr;
struct Entry { std::wstring name, skipReason; BY_HANDLE_FILE_INFORMATION identity{}; bool checked=false; std::wstring path; std::vector<BYTE> child; bool recycleBin=false; bool eligible=false; bool moved=false; };
std::vector<Entry> entries; std::wstring pathList;
IFolderView2* desktop=nullptr; size_t current=SIZE_MAX; bool dropReceived=false;
std::atomic<unsigned long long> audioRequest{0}, audioApplied{0};
std::atomic<ULONGLONG> audioEnd{0};
std::atomic<bool> audioDone{false};
void requestAudio(int id){
 auto old=audioRequest.load();
 while(!audioRequest.compare_exchange_weak(old,(((old>>8)+1)<<8)|static_cast<unsigned>(id))){}
}
void voice(int id){requestAudio(id);}
void intervention(){DWORD now=GetTickCount();if(!lastAttempt||now-lastAttempt>=6000){lastAttempt=now;attempts.fetch_add(1);}}
LRESULT CALLBACK keyHook(int n,WPARAM w,LPARAM l){
 if(n>=0){auto* k=reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
  if(k->dwExtraInfo==tag)return CallNextHookEx(keyboard,n,w,l);
  bool down=w==WM_KEYDOWN||w==WM_SYSKEYDOWN;
  if(blocked){if(down)intervention();return 1;}
 }return CallNextHookEx(keyboard,n,w,l);
}
LRESULT CALLBACK mouseHook(int n,WPARAM w,LPARAM l){
 if(n>=0&&blocked){auto* m=reinterpret_cast<MSLLHOOKSTRUCT*>(l);if(m->dwExtraInfo!=tag){intervention();return 1;}}
 return CallNextHookEx(mouse,n,w,l);
}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
void waitMs(DWORD ms){ULONGLONG until=GetTickCount64()+ms;do{pump();Sleep(5);}while(GetTickCount64()<until);}
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
  if(blocked){unsigned count=attempts.load();if(count!=observed){observed=count;voice(first?3:11);first=false;}}
  auto request=audioRequest.load();
  if(request!=audioApplied.load()){
   int id=static_cast<int>(request&255);HRSRC r=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
   const BYTE* p=r?static_cast<const BYTE*>(LockResource(LoadResource(nullptr,r))):nullptr;
   DWORD duration=p?wavDuration(p,SizeofResource(nullptr,r)):0;
   PlaySoundW(nullptr,nullptr,0);
   bool ok=p&&duration&&PlaySoundW(reinterpret_cast<LPCWSTR>(p),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
   audioEnd=GetTickCount64()+(ok?duration:0);audioApplied=request;
  }
  Sleep(5);
 }
 PlaySoundW(nullptr,nullptr,0);
}
void waitAudio(){
 do{waitMs(5);}while(audioApplied.load()!=audioRequest.load()||GetTickCount64()<audioEnd.load());
}

std::vector<BYTE> pidlBytes(PCUIDLIST_RELATIVE p){UINT n=ILGetSize(p);return std::vector<BYTE>(reinterpret_cast<const BYTE*>(p),reinterpret_cast<const BYTE*>(p)+n);}
PCUITEMID_CHILD childPidl(const std::vector<BYTE>& v){return reinterpret_cast<PCUITEMID_CHILD>(v.data());}
void getDesktop(){
 Ptr<IShellWindows> windows;check(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(windows.out())),"Cannot access Explorer");
 VARIANT location,empty;VariantInit(&location);VariantInit(&empty);location.vt=VT_I4;location.lVal=CSIDL_DESKTOP;long hwnd=0;Ptr<IDispatch> dispatch;
 check(windows->FindWindowSW(&location,&empty,SWC_DESKTOP,&hwnd,SWFO_NEEDDISPATCH,dispatch.out()),"Desktop not found");
 if(!dispatch)throw std::runtime_error("Explorer returned no desktop dispatch");
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
 if((attrs&FILE_ATTRIBUTE_DIRECTORY)&&!probeTree(e.path,e.skipReason))return false;
 if(record){e.identity=info;e.checked=true;}e.skipReason.clear();return true;
}
void writePathList(){
 // Preserve original paths as requested. No restore commands or diagnostic sidecars.
 std::wstring text=L"status\tname\toriginal_path\treason\n";
 for(auto& e:entries)text+=(e.eligible?L"READY":L"SKIP")+std::wstring(L"\t")+e.name+L"\t"+e.path+L"\t"+e.skipReason+L"\n";
 int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
 std::string bytes(size,'\0');WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),bytes.data(),size,nullptr,nullptr);
 HANDLE h=CreateFileW(pathList.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot save original paths");
 DWORD written=0;bool ok=WriteFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(h);CloseHandle(h);
 if(!ok)throw std::runtime_error("Cannot flush original paths");
}
void snapshot(){
 Ptr<IShellFolder> folder;check(desktop->GetFolder(IID_PPV_ARGS(folder.out())),"Cannot access desktop folder");
 PWSTR publicPath=nullptr;check(SHGetKnownFolderPath(FOLDERID_PublicDesktop,0,nullptr,&publicPath),"Public desktop path unavailable");std::wstring publicRoot=publicPath;CoTaskMemFree(publicPath);
 PWSTR root=nullptr;check(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&root),"Desktop path unavailable");std::wstring userRoot=root;CoTaskMemFree(root);
 int count=0;check(desktop->ItemCount(SVGIO_ALLVIEW,&count),"Cannot enumerate icons");
 for(int i=0;i<count;i++){PITEMID_CHILD child=nullptr;if(FAILED(desktop->Item(i,&child))||!child)continue;Entry e;e.child=pidlBytes(child);
 Ptr<IShellItem> item;HRESULT hr=SHCreateItemWithParent(nullptr,folder,child,IID_PPV_ARGS(item.out()));CoTaskMemFree(child);if(FAILED(hr))continue;e.path=display(item,SIGDN_FILESYSPATH);e.name=display(item,SIGDN_NORMALDISPLAY);e.skipReason=L"Виртуальный или системный значок";
 std::wstring parsing=display(item,SIGDN_DESKTOPABSOLUTEPARSING);e.recycleBin=_wcsicmp(parsing.c_str(),L"::{645FF040-5081-101B-9F08-00AA002F954E}")==0;
 if(!e.path.empty()){size_t slash=e.path.find_last_of(L"\\/");std::wstring parent=e.path.substr(0,slash);DWORD attrs=GetFileAttributesW(e.path.c_str());
 wchar_t volume[MAX_PATH]{};bool local=GetVolumePathNameW(e.path.c_str(),volume,MAX_PATH)&&GetDriveTypeW(volume)==DRIVE_FIXED;
 bool onDesktop=_wcsicmp(parent.c_str(),userRoot.c_str())==0||_wcsicmp(parent.c_str(),publicRoot.c_str())==0;
 e.eligible=local&&onDesktop&&attrs!=INVALID_FILE_ATTRIBUTES;
 if(e.eligible)e.eligible=probe(e,true);else e.skipReason=L"Недоступный путь или не локальный рабочий стол";}
 entries.push_back(std::move(e));}
 PWSTR local=nullptr;check(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local),"LocalAppData unavailable");std::wstring dir=std::wstring(local)+L"\\DesktopPrank";CoTaskMemFree(local);CreateDirectoryW(dir.c_str(),nullptr);
 SYSTEMTIME t;GetLocalTime(&t);wchar_t name[96];swprintf(name,96,L"\\paths-%04u%02u%02u-%02u%02u%02u-%lu.tsv",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId());pathList=dir+name;writePathList();
}
struct Sink : IFileOperationProgressSink {
 ULONG refs=1; Entry* entry; HRESULT result=E_FAIL; bool captured=false;
 explicit Sink(Entry* e):entry(e){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p)override{if(id==IID_IUnknown||id==IID_IFileOperationProgressSink){*p=this;AddRef();return S_OK;}*p=nullptr;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{return --refs;}
 HRESULT STDMETHODCALLTYPE StartOperations()override{return S_OK;}HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD,IShellItem*,LPCWSTR)override{return E_ABORT;}
 HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR)override{return E_ABORT;}
 HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR)override{return E_ABORT;}
 HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT,IShellItem*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags,IShellItem*)override{
 // Fail closed if Shell proposes permanent deletion, even if the bin is disabled/full.
 return (flags&TSF_DELETE_RECYCLE_IF_POSSIBLE)?S_OK:E_ABORT;
 }
 HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,IShellItem*,HRESULT h,IShellItem* recycled)override{
 result=h;if(SUCCEEDED(h)&&recycled){entry->moved=true;captured=true;}return S_OK;
 }
 HRESULT STDMETHODCALLTYPE PreNewItem(DWORD,IShellItem*,LPCWSTR)override{return E_ABORT;}
 HRESULT STDMETHODCALLTYPE PostNewItem(DWORD,IShellItem*,LPCWSTR,LPCWSTR,DWORD,HRESULT,IShellItem*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE UpdateProgress(UINT,UINT)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE ResetTimer()override{return S_OK;}HRESULT STDMETHODCALLTYPE PauseTimer()override{return S_OK;}HRESULT STDMETHODCALLTYPE ResumeTimer()override{return S_OK;}
};
void recycle(Entry& e){
 if(!probe(e,false)){e.eligible=false;return;}
 Ptr<IShellItem> item;check(SHCreateItemFromParsingName(e.path.c_str(),nullptr,IID_PPV_ARGS(item.out())),"Item disappeared before drag");
 Ptr<IFileOperation> op;check(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(op.out())),"File operation unavailable");
 check(op->SetOperationFlags(FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_NOERRORUI|FOF_SILENT|FOF_NO_CONNECTED_ELEMENTS|FOFX_RECYCLEONDELETE|FOFX_EARLYFAILURE),"Cannot set recycle flags");
 Sink sink(&e);check(op->DeleteItem(item,&sink),"Cannot schedule recycle");HRESULT h=op->PerformOperations();BOOL aborted=FALSE;op->GetAnyOperationsAborted(&aborted);
 if(FAILED(h)||aborted||FAILED(sink.result)||!sink.captured)throw std::runtime_error("Recycle failed or no recovery identity was returned");
}
struct DropTarget : IDropTarget {
 ULONG refs=1;bool acceptable=false;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p)override{if(id==IID_IUnknown||id==IID_IDropTarget){*p=this;AddRef();return S_OK;}*p=nullptr;return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{return --refs;}
 bool matches(IDataObject* data){if(current>=entries.size())return false;Ptr<IShellItemArray> array;if(FAILED(SHCreateShellItemArrayFromDataObject(data,IID_PPV_ARGS(array.out()))))return false;DWORD count=0;array->GetCount(&count);if(count!=1)return false;Ptr<IShellItem> item;if(FAILED(array->GetItemAt(0,item.out())))return false;return _wcsicmp(display(item,SIGDN_FILESYSPATH).c_str(),entries[current].path.c_str())==0;}
 HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data,DWORD,POINTL,DWORD* effect)override{acceptable=matches(data);*effect=acceptable?DROPEFFECT_MOVE:DROPEFFECT_NONE;return S_OK;}
 HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD* effect)override{*effect=acceptable?DROPEFFECT_MOVE:DROPEFFECT_NONE;return S_OK;}
 HRESULT STDMETHODCALLTYPE DragLeave()override{acceptable=false;return S_OK;}
 HRESULT STDMETHODCALLTYPE Drop(IDataObject* data,DWORD,POINTL,DWORD* effect)override{
 *effect=DROPEFFECT_NONE;dropReceived=true;if(!matches(data))return E_ABORT;
 try{recycle(entries[current]);
 // The target has already moved the item. Never ask Explorer to delete the source again.
 FORMATETC format{};format.cfFormat=static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));format.dwAspect=DVASPECT_CONTENT;format.lindex=-1;format.tymed=TYMED_HGLOBAL;
 STGMEDIUM medium{};medium.tymed=TYMED_HGLOBAL;medium.hGlobal=GlobalAlloc(GMEM_MOVEABLE,sizeof(DWORD));
 if(medium.hGlobal){auto* value=static_cast<DWORD*>(GlobalLock(medium.hGlobal));if(value){*value=DROPEFFECT_NONE;GlobalUnlock(medium.hGlobal);if(FAILED(data->SetData(&format,&medium,TRUE)))ReleaseStgMedium(&medium);}else GlobalFree(medium.hGlobal);}
 *effect=DROPEFFECT_NONE;
 }catch(...){auto& entry=entries[current];entry.eligible=false;entry.skipReason=L"Shell отклонила перенос";return E_FAIL;}return S_OK;
 }
};
LRESULT CALLBACK windowProc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;return DefWindowProcW(h,m,w,l);}
void mouseInput(DWORD flags,POINT p){INPUT in{};in.type=INPUT_MOUSE;in.mi.dwFlags=flags;in.mi.dwExtraInfo=tag;
 if(flags&MOUSEEVENTF_MOVE){int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN),width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);in.mi.dx=static_cast<LONG>((65536ll*(p.x-x)+32768)/std::max(1,width));in.mi.dy=static_cast<LONG>((65536ll*(p.y-y)+32768)/std::max(1,height));in.mi.dwFlags|=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;}
 if(SendInput(1,&in,sizeof(in))!=1)throw std::runtime_error("Mouse injection failed");
}
void glide(POINT from,POINT to){double distance=std::hypot(double(to.x-from.x),double(to.y-from.y));int steps=std::max(1,int(std::ceil(distance/13.5)));
 for(int i=1;i<=steps;i++){POINT p{from.x+LONG(std::lround(double(to.x-from.x)*i/steps)),from.y+LONG(std::lround(double(to.y-from.y)*i/steps))};mouseInput(MOUSEEVENTF_MOVE,p);waitMs(25);}}
struct DragRelease {
 bool pressed=false;
 ~DragRelease(){if(!pressed)return;INPUT in[3]{};for(auto& x:in)x.type=INPUT_KEYBOARD;
 in[0].ki.wVk=VK_ESCAPE;in[0].ki.dwExtraInfo=tag;in[1].ki.wVk=VK_ESCAPE;in[1].ki.dwFlags=KEYEVENTF_KEYUP;in[1].ki.dwExtraInfo=tag;
 in[2].type=INPUT_MOUSE;in[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;in[2].mi.dwExtraInfo=tag;SendInput(3,in,sizeof(INPUT));}
};
void drag(size_t index,POINT bin){
 if(!probe(entries[index],false)){entries[index].eligible=false;return;}
 current=index;dropReceived=false;POINT start=iconCenter(entries[index]);
 auto selected=childPidl(entries[index].child);
 check(desktop->SelectAndPositionItems(1,&selected,nullptr,SVSI_SELECT|SVSI_DESELECTOTHERS),"Cannot select source icon");
 POINT cursor;GetCursorPos(&cursor);glide(cursor,start);
 DragRelease release;mouseInput(MOUSEEVENTF_LEFTDOWN,start);release.pressed=true;waitMs(80);glide(start,bin);

 mouseInput(MOUSEEVENTF_LEFTUP,bin);release.pressed=false;waitMs(500);
 if(!dropReceived)throw std::runtime_error("Explorer did not deliver the mouse drop");
 current=SIZE_MAX;
}
POINT recyclePoint(){
 for(auto& e:entries)if(e.recycleBin)return iconCenter(e);
 throw std::runtime_error("Recycle Bin icon is not visible on the desktop");
}
BOOL CALLBACK minimizeWindow(HWND h,LPARAM){
 if(!IsWindowVisible(h)||h==targetWindow||h==GetShellWindow()||GetWindow(h,GW_OWNER))return TRUE;
 wchar_t cls[128]{};GetClassNameW(h,cls,128);
 if(wcscmp(cls,L"Shell_TrayWnd")==0||wcscmp(cls,L"Shell_SecondaryTrayWnd")==0||wcscmp(cls,L"WorkerW")==0||wcscmp(cls,L"Progman")==0)return TRUE;
 if(GetWindowLongPtrW(h,GWL_STYLE)&WS_CAPTION)ShowWindowAsync(h,SW_FORCEMINIMIZE);
 return TRUE;
}
void showDesktop(){EnumWindows(minimizeWindow,0);waitMs(700);}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
 BOOL(WINAPI* dpi)(HANDLE)=nullptr;auto proc=GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext");static_assert(sizeof(dpi)==sizeof(proc));memcpy(&dpi,&proc,sizeof(dpi));if(dpi)dpi(reinterpret_cast<HANDLE>(-4));
 if(FAILED(OleInitialize(nullptr)))return 1;
 std::thread audio, hooks;DropTarget target;bool registered=false;int status=0;
 try{
 getDesktop();snapshot();POINT bin=recyclePoint();
 WNDCLASSW cls{};cls.hInstance=instance;cls.lpfnWndProc=windowProc;cls.lpszClassName=L"DesktopPrank.Drop";RegisterClassW(&cls);
 targetWindow=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,cls.lpszClassName,L"",WS_POPUP,bin.x-24,bin.y-24,64,64,nullptr,nullptr,instance,nullptr);
 if(!targetWindow)throw std::runtime_error("Cannot create drop target");
 if(!SetLayeredWindowAttributes(targetWindow,0,1,LWA_ALPHA))throw std::runtime_error("Cannot hide drop target");
 check(RegisterDragDrop(targetWindow,&target),"Cannot register drop target");registered=true;
 HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!ready)throw std::runtime_error("Cannot create hook event");
 hooks=std::thread([instance,ready]{
  keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,keyHook,instance,0);mouse=SetWindowsHookExW(WH_MOUSE_LL,mouseHook,instance,0);SetEvent(ready);
  while(!done){pump();Sleep(5);}
  if(keyboard)UnhookWindowsHookEx(keyboard);
  if(mouse)UnhookWindowsHookEx(mouse);
 });
 DWORD result=WaitForSingleObject(ready,5000);CloseHandle(ready);
 if(result!=WAIT_OBJECT_0||!keyboard||!mouse)throw std::runtime_error("Cannot install input hooks");
 audio=std::thread(audioLoop);
 showDesktop();ShowWindow(targetWindow,SW_SHOWNOACTIVATE);blocked=true;
 voice(1);waitAudio();voice(2);waitAudio();
 size_t total=std::count_if(entries.begin(),entries.end(),[](const Entry& e){return e.eligible;});if(total>=15)voice(10);
 unsigned moved=0;
 for(size_t i=0;i<entries.size();i++){
  auto& e=entries[i];if(!e.eligible)continue;
  if(!probe(e,false)){e.eligible=false;continue;}
  DWORD attrs=GetFileAttributesW(e.path.c_str());bool folder=attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_DIRECTORY);
  if(folder&&e.name==L"Новая папка"){voice(6);waitAudio();}
  if(folder)voice(7);
  if(moved==3){voice(9);waitMs(1100);}
  // Auto-arrange can move both source and bin after each successful operation.
  bin=recyclePoint();SetWindowPos(targetWindow,HWND_TOPMOST,bin.x-24,bin.y-24,64,64,SWP_NOACTIVATE);
  try{drag(i,bin);}catch(const std::exception&){e.eligible=false;}
  if(e.moved){++moved;if(moved==1)voice(4);else if(moved==3)voice(8);else voice(5);}
 }
 blocked=false;ShowWindow(targetWindow,SW_HIDE);voice(13);waitAudio();voice(15);waitAudio();
 }catch(const std::exception&){blocked=false;status=1;}
 blocked=false;done=true;if(hooks.joinable())hooks.join();audioDone=true;if(audio.joinable())audio.join();
 if(registered)RevokeDragDrop(targetWindow);
 if(targetWindow)DestroyWindow(targetWindow);
 if(desktop)desktop->Release();
 OleUninitialize();return status;
}

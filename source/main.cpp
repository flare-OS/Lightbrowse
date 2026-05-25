// LightBrowse Legacy - main.cpp
// A lightweight browser using IE/Trident engine
//
// Compile:
//   g++ browser.cpp -o browser.exe -lole32 -loleaut32 -luuid -lshlwapi -lcomctl32 -lcomdlg32 -lgdi32
//
// Place icon.ico, newtab.html in the same folder as browser.exe
// Bookmarks saved to bookmarks.txt, history to history.txt (next to exe)
//
// Shortcuts: Ctrl+T=new tab, Ctrl+W=close tab, Ctrl+Tab/Shift=switch tabs,
//            Ctrl+L/F6=address bar, Ctrl+R/F5=refresh, Esc=stop,
//            Ctrl+D=bookmark, Ctrl+B=bookmarks, Ctrl+H=history,
//            Alt+Left=back, Alt+Right=forward
// Goodluck with compiling ;)

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <exdispid.h>
#include <ole2.h>
#include <oleidl.h>
#include <mshtmhst.h>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <shellapi.h>

// ---------------------------------------------------------------------------
// IDs
// ---------------------------------------------------------------------------
#define ID_BACK         101
#define ID_FORWARD      102
#define ID_REFRESH      103
#define ID_STOP         104
#define ID_ADDRESS      105
#define ID_GO           106
#define ID_STATUS       107
#define ID_TABS         108
#define ID_NEWTAB       109
#define ID_BOOKMARKS    110
#define ID_HISTORY      111
#define ID_DOWNLOADS    112
#define ID_CLOSETAB     113
#define IDM_BM_ADD      201
#define IDM_BM_OPEN     202
#define IDM_BM_DEL      203
#define IDM_HI_OPEN     204
#define IDM_HI_CLEAR    205
#define IDM_DL_CLEAR    206
#define IDM_DL_OPEN     207
#define ID_DL_LIST      301
#define ID_POPUP_LIST   302

static const int kTabH   = 26;
static const int kToolH  = 30;
static const int kStatH  = 22;
static const int kTabXW  = 16;  // width of X button area in tab

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------
struct Bookmark     { std::wstring title, url; };
struct HistoryEntry { std::wstring title, url; std::time_t when; };
struct Download {
    std::wstring filename, path, url;
    DWORD received, total;
    bool done, failed;
};

static std::vector<Bookmark>     g_bookmarks;
static std::vector<HistoryEntry> g_history;
static std::vector<Download>     g_downloads;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HWND      g_hwnd     = nullptr;
static HWND      g_hTabs    = nullptr;
static HWND      g_hAddress = nullptr;
static HWND      g_hStatus  = nullptr;
static HWND      g_hDlWnd   = nullptr;  // downloads popup
static HINSTANCE g_hInst    = nullptr;
static int       g_activeTab= -1;
static HACCEL    g_accel    = nullptr;
static WNDPROC   g_addrOldProc = nullptr;  // for address bar subclass

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
struct Tab;
static std::vector<Tab*> g_tabs;

static void     TabActivate(int idx);
static void     NavigateTab(int idx, const wchar_t* url);
static void     ResizeTab(int idx);
static void     UpdateTitle(int idx);
static void     RedrawTabs();
static RECT     BrowserRect();
static Tab*     ActiveTab();

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
static std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH]={};
    GetModuleFileNameW(nullptr,buf,MAX_PATH);
    wchar_t* sl=wcsrchr(buf,L'\\'); if(sl) *(sl+1)=L'\0';
    return buf;
}

static const wchar_t* NewTabURL()
{
    static wchar_t s[MAX_PATH+16]={};
    if(s[0]) return s;
    std::wstring d=ExeDir();
    wcscpy(s,L"file:///");
    wcscat(s,d.c_str());
    wcscat(s,L"newtab.html");
    for(wchar_t* p=s+8;*p;p++) if(*p==L'\\') *p=L'/';
    return s;
}

// ---------------------------------------------------------------------------
// Persistence: bookmarks & history
// ---------------------------------------------------------------------------
static void SaveBookmarks()
{
    std::wofstream f((ExeDir()+L"bookmarks.txt").c_str());
    for(auto& b:g_bookmarks)
        f<<b.title<<L"\n"<<b.url<<L"\n";
}

static void LoadBookmarks()
{
    std::wifstream f((ExeDir()+L"bookmarks.txt").c_str());
    if(!f) return;
    std::wstring title,url;
    while(std::getline(f,title)&&std::getline(f,url))
        if(!url.empty()) g_bookmarks.push_back({title,url});
}

static void SaveHistory()
{
    // Keep last 500 entries
    int start=(int)g_history.size()>500?(int)g_history.size()-500:0;
    std::wofstream f((ExeDir()+L"history.txt").c_str());
    for(int i=start;i<(int)g_history.size();i++)
        f<<g_history[i].when<<L"\n"<<g_history[i].title<<L"\n"<<g_history[i].url<<L"\n";
}

static void LoadHistory()
{
    std::wifstream f((ExeDir()+L"history.txt").c_str());
    if(!f) return;
    std::wstring wline,title,url;
    while(std::getline(f,wline)&&std::getline(f,title)&&std::getline(f,url)){
        HistoryEntry he; he.when=(std::time_t)_wtoi64(wline.c_str());
        he.title=title; he.url=url;
        g_history.push_back(he);
    }
}

// ---------------------------------------------------------------------------
// OleContainer
// ---------------------------------------------------------------------------
class OleContainer
    : public IOleClientSite
    , public IOleInPlaceSite
    , public IOleInPlaceFrame
{
    LONG m_ref=1;
    HWND m_hwnd;
    IOleInPlaceActiveObject* m_ipa=nullptr;
public:
    explicit OleContainer(HWND h):m_hwnd(h){}
    HRESULT __stdcall QueryInterface(REFIID r,void** p) override {
        if(r==IID_IUnknown)            {*p=static_cast<IOleClientSite*>(this);}
        else if(r==IID_IOleClientSite)  {*p=static_cast<IOleClientSite*>(this);}
        else if(r==IID_IOleInPlaceSite) {*p=static_cast<IOleInPlaceSite*>(this);}
        else if(r==IID_IOleInPlaceFrame){*p=static_cast<IOleInPlaceFrame*>(this);}
        else if(r==IID_IOleInPlaceUIWindow){*p=static_cast<IOleInPlaceFrame*>(this);}
        else{*p=nullptr;return E_NOINTERFACE;}
        AddRef();return S_OK;
    }
    ULONG __stdcall AddRef()  override{return InterlockedIncrement(&m_ref);}
    ULONG __stdcall Release() override{LONG r=InterlockedDecrement(&m_ref);if(!r)delete this;return r;}
    HRESULT __stdcall SaveObject() override{return S_OK;}
    HRESULT __stdcall GetMoniker(DWORD,DWORD,IMoniker**)override{return E_NOTIMPL;}
    HRESULT __stdcall GetContainer(IOleContainer**)override{return E_NOINTERFACE;}
    HRESULT __stdcall ShowObject()override{return S_OK;}
    HRESULT __stdcall OnShowWindow(BOOL)override{return S_OK;}
    HRESULT __stdcall RequestNewObjectLayout()override{return E_NOTIMPL;}
    HRESULT __stdcall GetWindow(HWND* p)override{*p=m_hwnd;return S_OK;}
    HRESULT __stdcall ContextSensitiveHelp(BOOL)override{return E_NOTIMPL;}
    HRESULT __stdcall CanInPlaceActivate()override{return S_OK;}
    HRESULT __stdcall OnInPlaceActivate()override{return S_OK;}
    HRESULT __stdcall OnUIActivate()override{return S_OK;}
    HRESULT __stdcall GetWindowContext(IOleInPlaceFrame** pF,IOleInPlaceUIWindow** pU,
                                      RECT* rP,RECT* rC,OLEINPLACEFRAMEINFO* fi)override{
        *pF=static_cast<IOleInPlaceFrame*>(this);AddRef();
        *pU=nullptr;*rP=BrowserRect();*rC=*rP;
        fi->cb=sizeof(OLEINPLACEFRAMEINFO);fi->fMDIApp=FALSE;
        fi->hwndFrame=m_hwnd;fi->haccel=nullptr;fi->cAccelEntries=0;return S_OK;
    }
    HRESULT __stdcall Scroll(SIZE)override{return S_OK;}
    HRESULT __stdcall OnUIDeactivate(BOOL)override{return S_OK;}
    HRESULT __stdcall OnInPlaceDeactivate()override{return S_OK;}
    HRESULT __stdcall DiscardUndoState()override{return S_OK;}
    HRESULT __stdcall DeactivateAndUndo()override{return S_OK;}
    HRESULT __stdcall OnPosRectChange(const RECT*)override{return S_OK;}
    HRESULT __stdcall GetBorder(RECT* r)override{GetClientRect(m_hwnd,r);return S_OK;}
    HRESULT __stdcall RequestBorderSpace(LPCBORDERWIDTHS)override{return INPLACE_E_NOTOOLSPACE;}
    HRESULT __stdcall SetBorderSpace(LPCBORDERWIDTHS)override{return S_OK;}
    HRESULT __stdcall SetActiveObject(IOleInPlaceActiveObject* ipa,LPCOLESTR)override{
        if(m_ipa)m_ipa->Release();m_ipa=ipa;if(m_ipa)m_ipa->AddRef();return S_OK;}
    HRESULT __stdcall InsertMenus(HMENU,LPOLEMENUGROUPWIDTHS)override{return S_OK;}
    HRESULT __stdcall SetMenu(HMENU,HOLEMENU,HWND)override{return S_OK;}
    HRESULT __stdcall RemoveMenus(HMENU)override{return S_OK;}
    HRESULT __stdcall SetStatusText(LPCOLESTR)override{return S_OK;}
    HRESULT __stdcall EnableModeless(BOOL)override{return S_OK;}
    HRESULT __stdcall TranslateAccelerator(MSG*,WORD)override{return E_NOTIMPL;}
    void TranslateKey(MSG* m){if(m_ipa)m_ipa->TranslateAccelerator(m);}
};

// ---------------------------------------------------------------------------
// EventSink (forward-declared before Tab)
// ---------------------------------------------------------------------------
class EventSink : public DWebBrowserEvents2
{
    LONG m_ref=1;
public:
    int m_tab;
    explicit EventSink(int t):m_tab(t){}
    HRESULT __stdcall QueryInterface(REFIID r,void** p)override{
        if(r==IID_IUnknown||r==IID_IDispatch||r==DIID_DWebBrowserEvents2)
        {*p=this;AddRef();return S_OK;}
        *p=nullptr;return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override{return InterlockedIncrement(&m_ref);}
    ULONG __stdcall Release()override{LONG r=InterlockedDecrement(&m_ref);if(!r)delete this;return r;}
    HRESULT __stdcall GetTypeInfoCount(UINT*)override{return E_NOTIMPL;}
    HRESULT __stdcall GetTypeInfo(UINT,LCID,ITypeInfo**)override{return E_NOTIMPL;}
    HRESULT __stdcall GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*)override{return E_NOTIMPL;}
    HRESULT __stdcall Invoke(DISPID id,REFIID,LCID,WORD,
                             DISPPARAMS* dp,VARIANT*,EXCEPINFO*,UINT*) override;
};

// ---------------------------------------------------------------------------
// Tab
// ---------------------------------------------------------------------------
struct Tab {
    IWebBrowser2* wb       =nullptr;
    OleContainer* container=nullptr;
    EventSink*    sink     =nullptr;
    DWORD         cookie   =0;
    std::wstring  url;
    std::wstring  title;
    std::wstring  pendingNav;  // tracks url from BeforeNavigate2 for download interception
    bool          loading  =false;
};

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
static RECT BrowserRect()
{
    RECT rc; GetClientRect(g_hwnd,&rc);
    rc.top+=kTabH+kToolH;
    rc.bottom-=kStatH;
    return rc;
}

static Tab* ActiveTab()
{
    if(g_activeTab<0||g_activeTab>=(int)g_tabs.size()) return nullptr;
    return g_tabs[g_activeTab];
}

static void UpdateTitle(int idx)
{
    if(idx<0||idx>=(int)g_tabs.size()) return;
    Tab* t=g_tabs[idx];
    if(idx==g_activeTab){
        std::wstring wt=t->title.empty()?L"LightBrowse Legacy":t->title+L" - LightBrowse Legacy";
        SetWindowTextW(g_hwnd,wt.c_str());
    }
    RedrawTabs();
}

static void ResizeTab(int idx)
{
    if(idx<0||idx>=(int)g_tabs.size()) return;
    Tab* t=g_tabs[idx]; if(!t->wb) return;
    RECT rc=BrowserRect();
    t->wb->put_Left(rc.left);t->wb->put_Top(rc.top);
    t->wb->put_Width(rc.right-rc.left);t->wb->put_Height(rc.bottom-rc.top);
}

// ---------------------------------------------------------------------------
// Custom tab strip (owner-drawn so we can add X buttons)
// ---------------------------------------------------------------------------
static const int kTabPad  = 10;  // horizontal padding inside tab
static const int kTabMinW = 80;
static const int kTabMaxW = 180;

static int TabWidth()
{
    if(g_tabs.empty()) return kTabMaxW;
    RECT rc; GetClientRect(g_hwnd,&rc);
    int available=rc.right-60; // leave room for + button
    int w=available/(int)g_tabs.size();
    if(w<kTabMinW) w=kTabMinW;
    if(w>kTabMaxW) w=kTabMaxW;
    return w;
}

// Returns the rect of the X button for tab idx
static RECT TabXRect(int idx)
{
    int tw=TabWidth();
    RECT r;
    r.left=idx*tw + tw - kTabXW - 4;
    r.right=r.left+kTabXW;
    r.top=4;
    r.bottom=kTabH-4;
    return r;
}

static void RedrawTabs()
{
    if(g_hwnd) InvalidateRect(g_hwnd,nullptr,FALSE);
}

static void DrawTabs(HDC hdc)
{
    RECT rc; GetClientRect(g_hwnd,&rc);
    int tw=TabWidth();
    int n=(int)g_tabs.size();

    // Tab strip background
    RECT stripRc={0,0,rc.right,kTabH};
    HBRUSH bgBr=CreateSolidBrush(RGB(180,200,220));
    FillRect(hdc,&stripRc,bgBr);
    DeleteObject(bgBr);

    for(int i=0;i<n;i++){
        Tab* t=g_tabs[i];
        bool active=(i==g_activeTab);
        RECT tr={i*tw,0,(i+1)*tw,kTabH};

        // Tab background
        HBRUSH tabBr;
        if(active) tabBr=CreateSolidBrush(RGB(240,246,252));
        else       tabBr=CreateSolidBrush(RGB(200,216,232));
        FillRect(hdc,&tr,tabBr);
        DeleteObject(tabBr);

        // Tab border
        HPEN pen=CreatePen(PS_SOLID,1,RGB(150,170,195));
        HPEN oldPen=(HPEN)SelectObject(hdc,pen);
        MoveToEx(hdc,tr.left,tr.top,nullptr); LineTo(hdc,tr.left,tr.bottom);
        MoveToEx(hdc,tr.right-1,tr.top,nullptr); LineTo(hdc,tr.right-1,tr.bottom);
        MoveToEx(hdc,tr.left,tr.top,nullptr); LineTo(hdc,tr.right,tr.top);
        if(!active){
            // bottom line for inactive tabs
            MoveToEx(hdc,tr.left,tr.bottom-1,nullptr); LineTo(hdc,tr.right,tr.bottom-1);
        }
        SelectObject(hdc,oldPen);
        DeleteObject(pen);

        // Tab title
        std::wstring label=t->title.empty()?L"New Tab":t->title;
        // Truncate to fit
        RECT txtRc={tr.left+kTabPad,tr.top+4,tr.right-kTabXW-6,tr.bottom-2};
        SetBkMode(hdc,TRANSPARENT);
        SetTextColor(hdc,active?RGB(0,40,90):RGB(50,70,100));
        HFONT fnt=CreateFontW(-12,0,0,0,active?FW_SEMIBOLD:FW_NORMAL,0,0,0,
                              DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        HFONT oldFnt=(HFONT)SelectObject(hdc,fnt);
        DrawTextW(hdc,label.c_str(),-1,&txtRc,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(hdc,oldFnt);
        DeleteObject(fnt);

        // X button
        RECT xr=TabXRect(i);
        HFONT xFnt=CreateFontW(-10,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        oldFnt=(HFONT)SelectObject(hdc,xFnt);
        SetTextColor(hdc,active?RGB(120,40,40):RGB(100,110,130));
        DrawTextW(hdc,L"x",-1,&xr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(hdc,oldFnt);
        DeleteObject(xFnt);
    }

    // + new tab button
    RECT plusRc={n*tw,2,n*tw+28,kTabH-2};
    if(plusRc.right<=rc.right){
        HBRUSH plusBr=CreateSolidBrush(RGB(210,225,240));
        FillRect(hdc,&plusRc,plusBr);
        DeleteObject(plusBr);
        HPEN pen=CreatePen(PS_SOLID,1,RGB(150,170,195));
        HPEN oldPen=(HPEN)SelectObject(hdc,pen);
        Rectangle(hdc,plusRc.left,plusRc.top,plusRc.right,plusRc.bottom);
        SelectObject(hdc,oldPen);
        DeleteObject(pen);
        SetBkMode(hdc,TRANSPARENT);
        SetTextColor(hdc,RGB(40,80,140));
        HFONT fnt=CreateFontW(-13,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        HFONT old=(HFONT)SelectObject(hdc,fnt);
        DrawTextW(hdc,L"+",-1,&plusRc,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(hdc,old);
        DeleteObject(fnt);
    }
}

// Hit-test the tab strip: returns tab index, or -1=miss, -2=+ button, -3=X on tab N
static int TabHitTest(int x,int y,int* xTabIdx=nullptr)
{
    if(y<0||y>=kTabH) return -1;
    int tw=TabWidth();
    int n=(int)g_tabs.size();
    // + button
    if(x>=n*tw&&x<n*tw+28) return -2;
    int idx=x/tw;
    if(idx<0||idx>=n) return -1;
    // X button?
    RECT xr=TabXRect(idx);
    if(x>=xr.left&&x<=xr.right&&y>=xr.top&&y<=xr.bottom){
        if(xTabIdx) *xTabIdx=idx;
        return -3;
    }
    return idx;
}

// ---------------------------------------------------------------------------
// EventSink::Invoke
// ---------------------------------------------------------------------------
HRESULT __stdcall EventSink::Invoke(DISPID id,REFIID,LCID,WORD,
                                    DISPPARAMS* dp,VARIANT*,EXCEPINFO*,UINT*)
{
    if(m_tab<0||m_tab>=(int)g_tabs.size()) return S_OK;
    Tab* t=g_tabs[m_tab];
    bool active=(m_tab==g_activeTab);

    switch(id)
    {
    case DISPID_DOWNLOADBEGIN:
        t->loading=true;
        if(active) SetWindowTextW(g_hStatus,L"Loading...");
        break;

    case DISPID_DOWNLOADCOMPLETE:
        t->loading=false;
        if(active) SetWindowTextW(g_hStatus,L"Done");
        break;

    case DISPID_NAVIGATECOMPLETE2:
        if(t->wb){
            BSTR loc=nullptr;
            if(SUCCEEDED(t->wb->get_LocationURL(&loc))&&loc){
                t->url=loc; SysFreeString(loc);
                if(active) SetWindowTextW(g_hAddress,t->url.c_str());
            }
            BSTR nm=nullptr;
            if(SUCCEEDED(t->wb->get_LocationName(&nm))&&nm){
                t->title=nm; SysFreeString(nm);
            } else t->title=t->url;

            // Add to history (skip file:// newtab)
            if(t->url.find(L"file://")==std::wstring::npos&&!t->url.empty()){
                HistoryEntry he; he.title=t->title; he.url=t->url; he.when=std::time(nullptr);
                g_history.push_back(he);
                if(g_history.size()>500) g_history.erase(g_history.begin());
                SaveHistory();
            }
            UpdateTitle(m_tab);
        }
        if(active) SetWindowTextW(g_hStatus,L"Done");
        break;

    case DISPID_PROGRESSCHANGE:
        if(active&&dp->cArgs>=2){
            long cur=dp->rgvarg[1].lVal,mx=dp->rgvarg[0].lVal;
            if(mx>0&&cur>=0){
                wchar_t buf[64];
                wsprintfW(buf,L"Loading... %ld%%",cur*100L/mx);
                SetWindowTextW(g_hStatus,buf);
            } else if(cur<0) SetWindowTextW(g_hStatus,L"Done");
        }
        break;

    case DISPID_STATUSTEXTCHANGE:
        if(active&&dp->cArgs>=1&&dp->rgvarg[0].vt==VT_BSTR&&dp->rgvarg[0].bstrVal)
            SetWindowTextW(g_hStatus,dp->rgvarg[0].bstrVal);
        break;

    case DISPID_COMMANDSTATECHANGE:
        if(active&&dp->cArgs>=2){
            int cmd=dp->rgvarg[1].lVal;
            BOOL en=dp->rgvarg[0].boolVal!=VARIANT_FALSE;
            if(cmd==CSC_NAVIGATEBACK)    EnableWindow(GetDlgItem(g_hwnd,ID_BACK),en);
            else if(cmd==CSC_NAVIGATEFORWARD) EnableWindow(GetDlgItem(g_hwnd,ID_FORWARD),en);
        }
        break;

    case DISPID_TITLECHANGE:
        if(dp->cArgs>=1&&dp->rgvarg[0].vt==VT_BSTR&&dp->rgvarg[0].bstrVal){
            t->title=dp->rgvarg[0].bstrVal;
            UpdateTitle(m_tab);
        }
        break;

    // dispid 250: track which URL we're about to navigate to (needed by FILEDOWNLOAD)
    case 250: // DISPID_BEFORENAVIGATE2
        if(dp->cArgs>=7){
            // args in reverse: [6]=pDisp [5]=URL [4]=Flags [3]=Target [2]=PostData [1]=Headers [0]=Cancel
            VARIANT* vUrl=&dp->rgvarg[5];
            BSTR bUrl=nullptr;
            if(vUrl->vt==VT_BSTR)              bUrl=vUrl->bstrVal;
            else if(vUrl->vt==(VT_BSTR|VT_BYREF)&&vUrl->pbstrVal) bUrl=*vUrl->pbstrVal;
            if(bUrl) t->pendingNav=bUrl;
        }
        break;

    // dispid 270: browser decided to download — cancel its dialog and use ours
    case 270: // DISPID_FILEDOWNLOAD
        if(dp->cArgs>=1&&(dp->rgvarg[0].vt==(VT_BOOL|VT_BYREF))&&dp->rgvarg[0].pboolVal){
            *dp->rgvarg[0].pboolVal=VARIANT_TRUE; // cancel browser download dialog
            if(!t->pendingNav.empty()){
                std::wstring url=t->pendingNav;
                t->pendingNav.clear();
                wchar_t* copy=new wchar_t[url.size()+1]; wcscpy(copy,url.c_str());
                PostMessageW(g_hwnd,WM_APP+3,0,(LPARAM)copy);
            }
        }
        break;

    case DISPID_NEWWINDOW3:
        if(dp->cArgs>=5&&dp->rgvarg[4].vt==VT_BSTR&&dp->rgvarg[4].bstrVal){
            std::wstring pu=dp->rgvarg[4].bstrVal;
            wchar_t* copy=new wchar_t[pu.size()+1]; wcscpy(copy,pu.c_str());
            PostMessageW(g_hwnd,WM_APP+1,0,(LPARAM)copy);
        }
        if(dp->cArgs>=4&&dp->rgvarg[3].vt==VT_BOOL)
            dp->rgvarg[3].boolVal=VARIANT_TRUE;
        break;
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Navigate
// ---------------------------------------------------------------------------
static void NavigateTab(int idx, const wchar_t* url)
{
    if(idx<0||idx>=(int)g_tabs.size()) return;
    Tab* t=g_tabs[idx]; if(!t->wb) return;
    if(!url||!url[0]) url=NewTabURL();

    wchar_t fixed[2200]={};
    if(!PathIsURLW(url)){
        bool hasSpace=wcschr(url,L' ')!=nullptr;
        bool hasDot  =wcschr(url,L'.')!=nullptr;
        if(hasSpace||!hasDot){
            wcscpy(fixed,L"https://www.google.com/search?q=");
            wcscat(fixed,url);
        } else {
            wcscpy(fixed,L"https://");
            wcscat(fixed,url);
        }
        url=fixed;
    }
    BSTR b=SysAllocString(url);
    VARIANT v; VariantInit(&v);
    t->wb->Navigate(b,&v,&v,&v,&v);
    SysFreeString(b);
}

// ---------------------------------------------------------------------------
// Tab create / activate / close
// ---------------------------------------------------------------------------
static int TabCreate(const wchar_t* url=nullptr)
{
    Tab* t=new Tab();
    int idx=(int)g_tabs.size();
    g_tabs.push_back(t);

    t->container=new OleContainer(g_hwnd);
    IOleObject* oleObj=nullptr;
    if(FAILED(CoCreateInstance(CLSID_WebBrowser,nullptr,CLSCTX_INPROC_SERVER,
                               IID_IOleObject,(void**)&oleObj))){
        return idx;
    }
    oleObj->SetClientSite(t->container);
    RECT rc=BrowserRect();
    oleObj->DoVerb(OLEIVERB_INPLACEACTIVATE,nullptr,t->container,0,g_hwnd,&rc);
    oleObj->QueryInterface(IID_IWebBrowser2,(void**)&t->wb);
    oleObj->Release();
    if(!t->wb) return idx;

    t->wb->put_AddressBar(VARIANT_FALSE);
    t->wb->put_MenuBar(VARIANT_FALSE);
    t->wb->put_ToolBar(VARIANT_FALSE);
    t->wb->put_StatusBar(VARIANT_FALSE);
    t->wb->put_Visible(VARIANT_TRUE);

    // Hide until activated
    IOleInPlaceObject* ipo=nullptr;
    if(SUCCEEDED(t->wb->QueryInterface(IID_IOleInPlaceObject,(void**)&ipo))){
        HWND hw=nullptr; ipo->GetWindow(&hw);
        if(hw) ShowWindow(hw,SW_HIDE);
        ipo->Release();
    }

    // Hook events
    t->sink=new EventSink(idx);
    IConnectionPointContainer* cpc=nullptr;
    if(SUCCEEDED(t->wb->QueryInterface(IID_IConnectionPointContainer,(void**)&cpc))){
        IConnectionPoint* cp=nullptr;
        if(SUCCEEDED(cpc->FindConnectionPoint(DIID_DWebBrowserEvents2,&cp))){
            cp->Advise(t->sink,&t->cookie); cp->Release();
        }
        cpc->Release();
    }
    NavigateTab(idx,url);
    RedrawTabs();
    return idx;
}

static void TabActivate(int idx)
{
    if(idx<0||idx>=(int)g_tabs.size()) return;
    // Hide old
    if(g_activeTab>=0&&g_activeTab<(int)g_tabs.size()){
        Tab* old=g_tabs[g_activeTab];
        if(old->wb){
            IOleInPlaceObject* ipo=nullptr;
            if(SUCCEEDED(old->wb->QueryInterface(IID_IOleInPlaceObject,(void**)&ipo))){
                HWND hw=nullptr; ipo->GetWindow(&hw);
                if(hw) ShowWindow(hw,SW_HIDE);
                ipo->Release();
            }
        }
    }
    g_activeTab=idx;
    Tab* t=g_tabs[idx];
    if(t->wb){
        ResizeTab(idx);
        IOleInPlaceObject* ipo=nullptr;
        if(SUCCEEDED(t->wb->QueryInterface(IID_IOleInPlaceObject,(void**)&ipo))){
            HWND hw=nullptr; ipo->GetWindow(&hw);
            if(hw) ShowWindow(hw,SW_SHOW);
            ipo->Release();
        }
        SetWindowTextW(g_hAddress,t->url.empty()?L"":t->url.c_str());
        EnableWindow(GetDlgItem(g_hwnd,ID_BACK),FALSE);
        EnableWindow(GetDlgItem(g_hwnd,ID_FORWARD),FALSE);
    }
    UpdateTitle(idx);
    RedrawTabs();
}

static void TabClose(int idx)
{
    if(idx<0||idx>=(int)g_tabs.size()) return;
    if(g_tabs.size()==1){ TabCreate(); }

    Tab* t=g_tabs[idx];
    if(t->wb&&t->cookie){
        IConnectionPointContainer* cpc=nullptr;
        if(SUCCEEDED(t->wb->QueryInterface(IID_IConnectionPointContainer,(void**)&cpc))){
            IConnectionPoint* cp=nullptr;
            if(SUCCEEDED(cpc->FindConnectionPoint(DIID_DWebBrowserEvents2,&cp))){
                cp->Unadvise(t->cookie); cp->Release();
            }
            cpc->Release();
        }
    }
    if(t->wb)        t->wb->Release();
    if(t->sink)      t->sink->Release();
    if(t->container) t->container->Release();
    delete t;
    g_tabs.erase(g_tabs.begin()+idx);
    for(int i=idx;i<(int)g_tabs.size();i++)
        if(g_tabs[i]->sink) g_tabs[i]->sink->m_tab=i;

    int nxt=idx<(int)g_tabs.size()?idx:(int)g_tabs.size()-1;
    g_activeTab=-1;
    TabActivate(nxt);
    RedrawTabs();
}

// ---------------------------------------------------------------------------
// Downloads popup
// ---------------------------------------------------------------------------
static void RefreshDlList()
{
    if(!g_hDlWnd) return;
    HWND lb=GetDlgItem(g_hDlWnd,ID_DL_LIST);
    if(!lb) return;
    SendMessageW(lb,LB_RESETCONTENT,0,0);
    for(auto& d:g_downloads){
        wchar_t buf[300];
        if(d.done)         wsprintfW(buf,L"[Done]    %s",d.filename.c_str());
        else if(d.failed)  wsprintfW(buf,L"[Failed]  %s",d.filename.c_str());
        else if(d.total>0) wsprintfW(buf,L"[%3ld%%]   %s",d.received*100/d.total,d.filename.c_str());
        else               wsprintfW(buf,L"[...]     %s",d.filename.c_str());
        SendMessageW(lb,LB_ADDSTRING,0,(LPARAM)buf);
    }
}

static LRESULT CALLBACK DlPopupProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    switch(m){
    case WM_SIZE:{
        RECT rc; GetClientRect(h,&rc);
        HWND lb=GetDlgItem(h,ID_DL_LIST);
        HWND btnClear=GetDlgItem(h,IDM_DL_CLEAR);
        HWND btnOpen =GetDlgItem(h,IDM_DL_OPEN);
        if(lb)       MoveWindow(lb,8,8,rc.right-16,rc.bottom-46,TRUE);
        if(btnOpen)  MoveWindow(btnOpen, 8,rc.bottom-32,90,26,TRUE);
        if(btnClear) MoveWindow(btnClear,106,rc.bottom-32,100,26,TRUE);
        break;}
    case WM_COMMAND:
        if(LOWORD(w)==IDM_DL_CLEAR){
            g_downloads.erase(std::remove_if(g_downloads.begin(),g_downloads.end(),
                [](const Download&d){return d.done||d.failed;}),g_downloads.end());
            RefreshDlList();
        } else if(LOWORD(w)==IDM_DL_OPEN){
            HWND lb=GetDlgItem(h,ID_DL_LIST);
            int sel=(int)SendMessageW(lb,LB_GETCURSEL,0,0);
            if(sel>=0&&sel<(int)g_downloads.size()&&g_downloads[sel].done)
                ShellExecuteW(nullptr,L"open",g_downloads[sel].path.c_str(),nullptr,nullptr,SW_SHOW);
        }
        break;
    case WM_CLOSE:
        ShowWindow(h,SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

static void ShowDownloadsPopup()
{
    if(!g_hDlWnd){
        WNDCLASSW wc={};
        wc.lpfnWndProc=DlPopupProc; wc.hInstance=g_hInst;
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
        wc.lpszClassName=L"LBDlPopup";
        RegisterClassW(&wc);

        g_hDlWnd=CreateWindowExW(WS_EX_TOOLWINDOW,L"LBDlPopup",
            L"Downloads - LightBrowse Legacy",
            WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME,
            200,200,480,320,g_hwnd,nullptr,g_hInst,nullptr);

        CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",nullptr,
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,
            8,8,448,226,g_hDlWnd,(HMENU)ID_DL_LIST,g_hInst,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Open File",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            8,242,90,26,g_hDlWnd,(HMENU)IDM_DL_OPEN,g_hInst,nullptr);
        CreateWindowExW(0,L"BUTTON",L"Clear Done",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            106,242,100,26,g_hDlWnd,(HMENU)IDM_DL_CLEAR,g_hInst,nullptr);
    }
    RefreshDlList();
    ShowWindow(g_hDlWnd,SW_SHOW);
    SetForegroundWindow(g_hDlWnd);
}

// ---------------------------------------------------------------------------
// Download thread (WinInet via dynamic load)
// ---------------------------------------------------------------------------
struct DlParam { int idx; std::wstring url, path; };

static DWORD WINAPI DownloadThread(LPVOID pv)
{
    DlParam* p=(DlParam*)pv;
    int idx=p->idx;
    std::wstring url=p->url, path=p->path;
    delete p;

    HMODULE hWI=LoadLibraryW(L"wininet.dll");
    if(!hWI){ g_downloads[idx].failed=true; PostMessageW(g_hwnd,WM_APP+2,0,0); return 0; }

    typedef void*(WINAPI*FnOpen)(LPCWSTR,DWORD,LPCWSTR,LPCWSTR,DWORD);
    typedef void*(WINAPI*FnOpenUrl)(void*,LPCWSTR,LPCWSTR,DWORD,DWORD,DWORD_PTR);
    typedef BOOL (WINAPI*FnRead)(void*,LPVOID,DWORD,LPDWORD);
    typedef BOOL (WINAPI*FnClose)(void*);
    typedef BOOL (WINAPI*FnQuery)(void*,DWORD,LPVOID,LPDWORD,LPDWORD);

    auto fnOpen   =(FnOpen)   GetProcAddress(hWI,"InternetOpenW");
    auto fnOpenUrl=(FnOpenUrl)GetProcAddress(hWI,"InternetOpenUrlW");
    auto fnRead   =(FnRead)   GetProcAddress(hWI,"InternetReadFile");
    auto fnClose  =(FnClose)  GetProcAddress(hWI,"InternetCloseHandle");
    auto fnQuery  =(FnQuery)  GetProcAddress(hWI,"HttpQueryInfoW");

    if(!fnOpen||!fnOpenUrl||!fnRead||!fnClose){
        g_downloads[idx].failed=true; PostMessageW(g_hwnd,WM_APP+2,0,0);
        FreeLibrary(hWI); return 0;
    }

    void* hInet=fnOpen(L"LightBrowseLegacy/1.0",0,nullptr,nullptr,0);
    void* hUrl =fnOpenUrl(hInet,url.c_str(),nullptr,0,0x80000000|0x04000000,0);
    if(!hUrl){
        g_downloads[idx].failed=true; PostMessageW(g_hwnd,WM_APP+2,0,0);
        fnClose(hInet); FreeLibrary(hWI); return 0;
    }
    if(fnQuery){
        wchar_t lb[32]={}; DWORD lsz=sizeof(lb),idx2=0;
        if(fnQuery(hUrl,5,lb,&lsz,&idx2))
            g_downloads[idx].total=(DWORD)_wtoi(lb);
    }
    HANDLE hFile=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,
                             CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(hFile==INVALID_HANDLE_VALUE){
        g_downloads[idx].failed=true; PostMessageW(g_hwnd,WM_APP+2,0,0);
        fnClose(hUrl);fnClose(hInet);FreeLibrary(hWI);return 0;
    }
    char buf[32768]; DWORD rd=0;
    while(fnRead(hUrl,(LPVOID)buf,sizeof(buf),&rd)&&rd>0){
        DWORD wr=0; WriteFile(hFile,buf,rd,&wr,nullptr);
        g_downloads[idx].received+=rd;
        PostMessageW(g_hwnd,WM_APP+2,0,0);
    }
    CloseHandle(hFile);
    fnClose(hUrl);fnClose(hInet);FreeLibrary(hWI);
    g_downloads[idx].done=true;
    PostMessageW(g_hwnd,WM_APP+2,0,0);
    return 0;
}

static void StartDownload(const std::wstring& url)
{
    std::wstring fname=url;
    size_t sl=fname.rfind(L'/'); if(sl!=std::wstring::npos) fname=fname.substr(sl+1);
    size_t q=fname.find(L'?');   if(q!=std::wstring::npos)  fname=fname.substr(0,q);
    if(fname.empty()||fname.size()>200) fname=L"download";

    wchar_t savePath[MAX_PATH]={}; wcscpy(savePath,fname.c_str());
    OPENFILENAMEW ofn={}; ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=g_hwnd; ofn.lpstrFile=savePath; ofn.nMaxFile=MAX_PATH;
    ofn.lpstrFilter=L"All Files\0*.*\0\0";
    ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    ofn.lpstrTitle=L"Save As";
    if(!GetSaveFileNameW(&ofn)) return;

    Download dl; dl.filename=fname; dl.path=savePath; dl.url=url;
    dl.received=0; dl.total=0; dl.done=false; dl.failed=false;
    g_downloads.push_back(dl);
    int di=(int)g_downloads.size()-1;

    ShowDownloadsPopup();

    DlParam* dp=new DlParam{di,url,savePath};
    CreateThread(nullptr,0,DownloadThread,dp,0,nullptr);
}

// ---------------------------------------------------------------------------
// Popup window for bookmarks / history
// ---------------------------------------------------------------------------
struct PopupCtx { bool isBookmarks; HWND lb; };

static LRESULT CALLBACK PopupProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    PopupCtx* ctx=(PopupCtx*)GetWindowLongPtrW(h,GWLP_USERDATA);
    switch(m){
    case WM_SIZE:{
        RECT rc; GetClientRect(h,&rc);
        HWND lb=GetDlgItem(h,ID_POPUP_LIST);
        if(lb) MoveWindow(lb,8,8,rc.right-16,rc.bottom-46,TRUE);
        HWND b1=GetDlgItem(h,IDM_BM_OPEN); if(!b1) b1=GetDlgItem(h,IDM_HI_OPEN);
        if(b1) MoveWindow(b1,8,rc.bottom-32,80,26,TRUE);
        HWND b2=GetDlgItem(h,IDM_BM_DEL);  if(!b2) b2=GetDlgItem(h,IDM_HI_CLEAR);
        if(b2) MoveWindow(b2,96,rc.bottom-32,90,26,TRUE);
        HWND bc=GetDlgItem(h,IDCANCEL); if(bc) MoveWindow(bc,rc.right-88,rc.bottom-32,80,26,TRUE);
        break;}
    case WM_COMMAND:{
        WORD id=LOWORD(w);
        HWND lb=GetDlgItem(h,ID_POPUP_LIST);
        if((id==IDM_BM_OPEN||id==IDM_HI_OPEN)||
           (id==ID_POPUP_LIST&&(HIWORD(w)==LBN_DBLCLK))){
            int sel=(int)SendMessageW(lb,LB_GETCURSEL,0,0);
            if(sel>=0){
                if(ctx->isBookmarks&&sel<(int)g_bookmarks.size())
                    NavigateTab(g_activeTab,g_bookmarks[sel].url.c_str());
                else if(!ctx->isBookmarks){
                    int ri=(int)g_history.size()-1-sel;
                    if(ri>=0) NavigateTab(g_activeTab,g_history[ri].url.c_str());
                }
                DestroyWindow(h);
            }
        } else if(id==IDM_BM_DEL&&ctx&&ctx->isBookmarks){
            int sel=(int)SendMessageW(lb,LB_GETCURSEL,0,0);
            if(sel>=0&&sel<(int)g_bookmarks.size()){
                g_bookmarks.erase(g_bookmarks.begin()+sel);
                SendMessageW(lb,LB_DELETESTRING,sel,0);
                SaveBookmarks();
            }
        } else if(id==IDM_HI_CLEAR&&ctx&&!ctx->isBookmarks){
            g_history.clear();
            SendMessageW(lb,LB_RESETCONTENT,0,0);
            SaveHistory();
        } else if(id==IDCANCEL) DestroyWindow(h);
        break;}
    case WM_DESTROY:
        delete ctx;
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

static void ShowListDialog(bool isBookmarks)
{
    const wchar_t* title=isBookmarks?L"Bookmarks - LightBrowse Legacy":L"History - LightBrowse Legacy";
    HWND dlg=CreateWindowExW(WS_EX_TOOLWINDOW,L"LBPopup",title,
        WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME,  // no WS_VISIBLE yet
        200,150,520,400,g_hwnd,nullptr,g_hInst,nullptr);
    if(!dlg) return;

    HWND lb=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",nullptr,
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        8,8,496,306,dlg,(HMENU)ID_POPUP_LIST,g_hInst,nullptr);

    if(isBookmarks){
        for(auto& b:g_bookmarks){
            std::wstring line=b.title+L"  ["+b.url+L"]";
            SendMessageW(lb,LB_ADDSTRING,0,(LPARAM)line.c_str());
        }
    } else {
        for(int i=(int)g_history.size()-1;i>=0;i--){
            auto& he=g_history[i];
            std::wstring line=he.title+L"  ["+he.url+L"]";
            SendMessageW(lb,LB_ADDSTRING,0,(LPARAM)line.c_str());
        }
    }

    WORD openId=isBookmarks?IDM_BM_OPEN:IDM_HI_OPEN;
    WORD actId =isBookmarks?IDM_BM_DEL :IDM_HI_CLEAR;
    const wchar_t* actLabel=isBookmarks?L"Delete":L"Clear All";

    CreateWindowExW(0,L"BUTTON",L"Open",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        8,322,80,26,dlg,(HMENU)(UINT_PTR)openId,g_hInst,nullptr);
    CreateWindowExW(0,L"BUTTON",actLabel,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        96,322,90,26,dlg,(HMENU)(UINT_PTR)actId,g_hInst,nullptr);
    CreateWindowExW(0,L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        424,322,80,26,dlg,(HMENU)IDCANCEL,g_hInst,nullptr);

    PopupCtx* ctx=new PopupCtx{isBookmarks,lb};
    SetWindowLongPtrW(dlg,GWLP_USERDATA,(LONG_PTR)ctx);
    SetWindowLongPtrW(dlg,GWLP_WNDPROC,(LONG_PTR)PopupProc);
}

// ---------------------------------------------------------------------------
// Address bar subclass � intercepts Enter & Backspace before browser sees them
// ---------------------------------------------------------------------------
static LRESULT CALLBACK AddrEditProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_KEYDOWN){
        if(w==VK_RETURN){
            wchar_t url[2200]={};
            GetWindowTextW(h,url,2200);
            NavigateTab(g_activeTab,url);
            return 0;
        }
        if(w==VK_ESCAPE){
            Tab* t=ActiveTab();
            SetWindowTextW(h,t?t->url.c_str():L"");
            SetFocus(g_hwnd);
            return 0;
        }
        // Let Backspace through to the edit control only (don't pass to browser)
        if(w==VK_BACK){
            // Call original edit proc directly � do NOT forward to browser
            return CallWindowProcW(g_addrOldProc,h,m,w,l);
        }
    }
    // For all other keys use default edit behaviour
    return CallWindowProcW(g_addrOldProc,h,m,w,l);
}

// ---------------------------------------------------------------------------
// Accelerators
// ---------------------------------------------------------------------------
static void BuildAccelerators()
{
    ACCEL ac[]={
        {FVIRTKEY|FCONTROL,'L',ID_ADDRESS},
        {FVIRTKEY,VK_F6,ID_ADDRESS},
        {FVIRTKEY|FCONTROL,'R',ID_REFRESH},
        {FVIRTKEY,VK_F5,ID_REFRESH},
        {FVIRTKEY|FCONTROL,'T',ID_NEWTAB},
        {FVIRTKEY|FCONTROL,'W',ID_CLOSETAB},
        {FVIRTKEY|FCONTROL,'D',IDM_BM_ADD},
        {FVIRTKEY|FCONTROL,'B',ID_BOOKMARKS},
        {FVIRTKEY|FCONTROL,'H',ID_HISTORY},
        {FVIRTKEY|FCONTROL,'J',ID_DOWNLOADS},
        {FVIRTKEY|FALT,VK_LEFT,ID_BACK},
        {FVIRTKEY|FALT,VK_RIGHT,ID_FORWARD},
        {FVIRTKEY,VK_ESCAPE,ID_STOP},
    };
    g_accel=CreateAcceleratorTableW(ac,sizeof(ac)/sizeof(ac[0]));
}

// ---------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
    switch(msg)
    {
    case WM_CREATE:
    {
        g_hwnd=hwnd;
        INITCOMMONCONTROLSEX icx={sizeof(icx),ICC_BAR_CLASSES};
        InitCommonControlsEx(&icx);
        BuildAccelerators();

        // Load icon
        HICON ico=(HICON)LoadImageW(nullptr,(ExeDir()+L"icon.ico").c_str(),
                                   IMAGE_ICON,0,0,LR_LOADFROMFILE|LR_DEFAULTSIZE);
        if(ico){ SendMessageW(hwnd,WM_SETICON,ICON_BIG,(LPARAM)ico);
                 SendMessageW(hwnd,WM_SETICON,ICON_SMALL,(LPARAM)ico); }

        // Toolbar buttons
        auto Btn=[&](const wchar_t* t,int x,int w,int id){
            return CreateWindowExW(0,L"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                x,kTabH,w,kToolH,hwnd,(HMENU)(UINT_PTR)id,nullptr,nullptr);
        };
        Btn(L"<",   0,  28,ID_BACK);
        Btn(L">",   28, 28,ID_FORWARD);
        Btn(L"R",   56, 28,ID_REFRESH);
        Btn(L"X",   84, 28,ID_STOP);

        g_hAddress=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            114,kTabH+2,760,kToolH-4,hwnd,(HMENU)ID_ADDRESS,nullptr,nullptr);

        // Subclass address bar to intercept Enter/Backspace
        g_addrOldProc=(WNDPROC)SetWindowLongPtrW(g_hAddress,GWLP_WNDPROC,(LONG_PTR)AddrEditProc);

        Btn(L"Go",  876, 36,ID_GO);
        Btn(L"*",   914, 28,ID_BOOKMARKS);
        Btn(L"H",   942, 28,ID_HISTORY);
        Btn(L"DL",  970, 32,ID_DOWNLOADS);

        g_hStatus=CreateWindowExW(0,STATUSCLASSNAMEW,L"Ready",
            WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
            0,0,0,0,hwnd,(HMENU)ID_STATUS,nullptr,nullptr);

        EnableWindow(GetDlgItem(hwnd,ID_BACK),FALSE);
        EnableWindow(GetDlgItem(hwnd,ID_FORWARD),FALSE);

        LoadBookmarks();
        LoadHistory();

        int i=TabCreate();
        TabActivate(i);
        break;
    }

    // Custom tab strip painting
    case WM_PAINT:{
        PAINTSTRUCT ps;
        HDC hdc=BeginPaint(hwnd,&ps);
        // Double-buffer to reduce flicker
        RECT rc; GetClientRect(hwnd,&rc);
        HDC memDC=CreateCompatibleDC(hdc);
        HBITMAP memBmp=CreateCompatibleBitmap(hdc,rc.right,kTabH);
        HBITMAP oldBmp=(HBITMAP)SelectObject(memDC,memBmp);
        DrawTabs(memDC);
        BitBlt(hdc,0,0,rc.right,kTabH,memDC,0,0,SRCCOPY);
        SelectObject(memDC,oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd,&ps);
        break;
    }

    case WM_LBUTTONDOWN:{
        int x=GET_X_LPARAM(lParam);
        int y=GET_Y_LPARAM(lParam);
        int xIdx=-1;
        int hit=TabHitTest(x,y,&xIdx);
        if(hit==-2){ // + button
            int i=TabCreate(); TabActivate(i); SetFocus(g_hAddress);
        } else if(hit==-3&&xIdx>=0){ // X button
            TabClose(xIdx);
        } else if(hit>=0){ // tab body
            if(hit!=g_activeTab) TabActivate(hit);
        }
        break;
    }

    case WM_COMMAND:{
        WORD id=LOWORD(wParam);
        if(id==ID_GO){
            wchar_t url[2200]; GetWindowTextW(g_hAddress,url,2200);
            NavigateTab(g_activeTab,url);
        }
        else if(id==ID_ADDRESS&&HIWORD(wParam)==EN_SETFOCUS)
            SendMessageW(g_hAddress,EM_SETSEL,0,-1);
        else if(id==ID_BACK)     { Tab*t=ActiveTab(); if(t&&t->wb) t->wb->GoBack(); }
        else if(id==ID_FORWARD)  { Tab*t=ActiveTab(); if(t&&t->wb) t->wb->GoForward(); }
        else if(id==ID_REFRESH)  { Tab*t=ActiveTab(); if(t&&t->wb) t->wb->Refresh(); }
        else if(id==ID_STOP)     { Tab*t=ActiveTab(); if(t&&t->wb) t->wb->Stop(); }
        else if(id==ID_NEWTAB)   { int i=TabCreate(); TabActivate(i); SetFocus(g_hAddress); }
        else if(id==ID_CLOSETAB) { TabClose(g_activeTab); }
        else if(id==ID_DOWNLOADS){ ShowDownloadsPopup(); }
        else if(id==IDM_BM_ADD){
            Tab*t=ActiveTab();
            if(t&&!t->url.empty()&&t->url.find(L"file://")==std::wstring::npos){
                Bookmark bm; bm.title=t->title; bm.url=t->url;
                if(bm.title.empty()) bm.title=bm.url;
                g_bookmarks.push_back(bm);
                SaveBookmarks();
                SetWindowTextW(g_hStatus,L"Bookmarked!");
            }
        }
        else if(id==ID_BOOKMARKS){ ShowListDialog(true); }
        else if(id==ID_HISTORY)  { ShowListDialog(false); }
        break;
    }

    case WM_KEYDOWN:{
        bool ctrl =(GetKeyState(VK_CONTROL)&0x8000)!=0;
        bool shift=(GetKeyState(VK_SHIFT)  &0x8000)!=0;
        if(ctrl&&wParam==VK_TAB){
            if(g_tabs.size()>1){
                int n=shift?(g_activeTab-1+(int)g_tabs.size())%(int)g_tabs.size()
                            :(g_activeTab+1)%(int)g_tabs.size();
                TabActivate(n);
            }
            return 0;
        }
        break;
    }

    case WM_SIZE:{
        int w=LOWORD(lParam),h=HIWORD(lParam);
        MoveWindow(GetDlgItem(hwnd,ID_BACK),    0,  kTabH,28,kToolH,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_FORWARD), 28, kTabH,28,kToolH,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_REFRESH), 56, kTabH,28,kToolH,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_STOP),    84, kTabH,28,kToolH,TRUE);
        MoveWindow(g_hAddress,114,kTabH+2,w-246,kToolH-4,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_GO),      w-130,kTabH,36,kToolH,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_BOOKMARKS),w-92,kTabH,28,kToolH,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_HISTORY), w-62,kTabH,28,kToolH,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_DOWNLOADS),w-32,kTabH,32,kToolH,TRUE);
        SendMessageW(g_hStatus,WM_SIZE,wParam,lParam);
        ResizeTab(g_activeTab);
        InvalidateRect(hwnd,nullptr,FALSE);
        break;
    }

    case WM_APP+1:{ // popup -> new tab
        wchar_t* url=(wchar_t*)lParam;
        int i=TabCreate(url); TabActivate(i);
        delete[] url;
        break;
    }
    case WM_APP+2: // download progress
        RefreshDlList();
        break;
    case WM_APP+3:{ // start download triggered from FileDownload event
        wchar_t* url=(wchar_t*)lParam;
        StartDownload(url);
        delete[] url;
        break;
    }

    case WM_DESTROY:{
        for(auto t:g_tabs){
            if(t->wb&&t->cookie){
                IConnectionPointContainer*cpc=nullptr;
                if(SUCCEEDED(t->wb->QueryInterface(IID_IConnectionPointContainer,(void**)&cpc))){
                    IConnectionPoint*cp=nullptr;
                    if(SUCCEEDED(cpc->FindConnectionPoint(DIID_DWebBrowserEvents2,&cp))){
                        cp->Unadvise(t->cookie);cp->Release();
                    }
                    cpc->Release();
                }
            }
            if(t->wb)        t->wb->Release();
            if(t->sink)      t->sink->Release();
            if(t->container) t->container->Release();
            delete t;
        }
        if(g_accel) DestroyAcceleratorTable(g_accel);
        SaveHistory();
        CoUninitialize();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

// ---------------------------------------------------------------------------
// Registry: IE11 emulation + TLS 1.2
// ---------------------------------------------------------------------------
static void SetBrowserEmulation()
{
    wchar_t ep[MAX_PATH]={}; GetModuleFileNameW(nullptr,ep,MAX_PATH);
    wchar_t*en=wcsrchr(ep,L'\\'); en=en?en+1:ep;
    struct{const wchar_t*k;DWORD v;}f[]={
        {L"SOFTWARE\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_BROWSER_EMULATION",11001},
        {L"SOFTWARE\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_GPU_RENDERING",1},
        {L"SOFTWARE\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_ENABLE_CLIPCHILDREN_OPTIMIZATION",1},
        {L"SOFTWARE\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_XMLHTTP",1},
    };
    for(auto&x:f){
        HKEY hk;
        if(RegCreateKeyExW(HKEY_CURRENT_USER,x.k,0,nullptr,REG_OPTION_NON_VOLATILE,
                           KEY_SET_VALUE,nullptr,&hk,nullptr)==ERROR_SUCCESS){
            RegSetValueExW(hk,en,0,REG_DWORD,(BYTE*)&x.v,sizeof(DWORD));
            RegCloseKey(hk);
        }
    }
    HKEY hk;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0,KEY_READ|KEY_SET_VALUE,&hk)==ERROR_SUCCESS){
        DWORD p=0,sz=sizeof(p);
        RegQueryValueExW(hk,L"SecureProtocols",nullptr,nullptr,(BYTE*)&p,&sz);
        p|=0x200|0x800;
        RegSetValueExW(hk,L"SecureProtocols",0,REG_DWORD,(BYTE*)&p,sizeof(p));
        RegCloseKey(hk);
    }
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int nCmdShow)
{
    SetBrowserEmulation();
    g_hInst=hInst;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);

    // Register main window class
    WNDCLASSW wc={}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpszClassName=L"LightBrowseLegacy";
    RegisterClassW(&wc);

    // Register popup window class (bookmarks/history)
    WNDCLASSW pc={}; pc.lpfnWndProc=DefWindowProcW; pc.hInstance=hInst;
    pc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    pc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    pc.lpszClassName=L"LBPopup";
    RegisterClassW(&pc);

    HWND hwnd=CreateWindowExW(0,L"LightBrowseLegacy",L"LightBrowse Legacy",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1100,720,
        nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,nCmdShow);
    UpdateWindow(hwnd);

    MSG msg={};
    while(GetMessageW(&msg,nullptr,0,0)){
        bool handled=false;

        // Intercept Enter/Backspace when address bar is focused � must come FIRST
        // before the browser's TranslateAccelerator can swallow them.
        if(msg.hwnd==g_hAddress||IsChild(g_hAddress,msg.hwnd)){
            if(msg.message==WM_KEYDOWN){
                if(msg.wParam==VK_RETURN){
                    wchar_t url[2200]={}; GetWindowTextW(g_hAddress,url,2200);
                    NavigateTab(g_activeTab,url);
                    handled=true;
                } else if(msg.wParam==VK_BACK){
                    // Pass Backspace directly to the edit � skip browser entirely
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                    handled=true;
                }
            }
        }

        if(!handled){
            Tab*t=ActiveTab();
            if(t&&t->wb){
                IOleInPlaceActiveObject*ipa=nullptr;
                if(SUCCEEDED(t->wb->QueryInterface(IID_IOleInPlaceActiveObject,(void**)&ipa))){
                    if(ipa->TranslateAccelerator(&msg)==S_OK) handled=true;
                    ipa->Release();
                }
            }
        }
        if(!handled&&g_accel&&TranslateAccelerator(hwnd,g_accel,&msg)) handled=true;
        if(!handled){ TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
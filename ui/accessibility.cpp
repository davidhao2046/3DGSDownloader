#include "accessibility.hpp"
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>

namespace gsdl::ui {
namespace {
struct State {
    std::mutex mutex;
    HWND window{};
    UINT message{};
    bool alive{true};
    std::vector<AccessibleNode> nodes;
};
CONTROLTYPEID control_type(AccessibleKind kind) {
    switch (kind) {
    case AccessibleKind::edit: return UIA_EditControlTypeId;
    case AccessibleKind::button: return UIA_ButtonControlTypeId;
    case AccessibleKind::list: return UIA_ListControlTypeId;
    case AccessibleKind::list_item: return UIA_ListItemControlTypeId;
    default: return UIA_TextControlTypeId;
    }
}
bool same_bounds(const RECT &a, const RECT &b) {
    return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
}
bool same_node(const AccessibleNode &a, const AccessibleNode &b) {
    return a.id==b.id && a.kind==b.kind && a.name==b.name && a.value==b.value &&
        a.focused==b.focused && a.enabled==b.enabled && same_bounds(a.screen_bounds,b.screen_bounds);
}
VARIANT boolean_value(bool value) {
    VARIANT result{};result.vt=VT_BOOL;result.boolVal=value?VARIANT_TRUE:VARIANT_FALSE;return result;
}
VARIANT string_value(const std::wstring &value) {
    VARIANT result{};result.vt=VT_BSTR;result.bstrVal=SysAllocString(value.c_str());return result;
}
VARIANT rectangle_value(const RECT &bounds) {
    VARIANT result{};
    auto array=SafeArrayCreateVector(VT_R8,0,4);
    if(!array)return result;
    double values[]{double(bounds.left),double(bounds.top),double(bounds.right-bounds.left),double(bounds.bottom-bounds.top)};
    for(LONG i=0;i<4;++i)SafeArrayPutElement(array,&i,&values[i]);
    result.vt=VT_ARRAY|VT_R8;result.parray=array;return result;
}
void property_changed(IRawElementProviderSimple *provider, PROPERTYID id, VARIANT before, VARIANT after) {
    UiaRaiseAutomationPropertyChangedEvent(provider,id,before,after);
    VariantClear(&before);VariantClear(&after);
}
class Provider final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IValueProvider,
                       public IInvokeProvider {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<State> state_;
    int id_{};
    Provider* root_{};
    std::optional<AccessibleNode> node() const {
        std::lock_guard lock(state_->mutex);
        if (!state_->alive) return {};
        if (id_ == 0) {
            RECT bounds{}; GetWindowRect(state_->window, &bounds);
            return AccessibleNode{0,L"3DGSDownloader",L"",bounds,false,true,AccessibleKind::text};
        }
        const auto it = std::find_if(state_->nodes.begin(),state_->nodes.end(),[&](const auto& n){return n.id==id_;});
        return it == state_->nodes.end() ? std::optional<AccessibleNode>{} : *it;
    }
    Provider* root() { return root_ ? root_ : this; }
    HRESULT post(AccessibleRequestKind kind, std::wstring value={}) {
        const auto n=node();
        if(!n)return UIA_E_ELEMENTNOTAVAILABLE;
        if(!n->enabled)return UIA_E_ELEMENTNOTENABLED;
        auto request=std::make_unique<AccessibleRequest>(AccessibleRequest{kind,id_,std::move(value)});
        std::lock_guard lock(state_->mutex);
        if(!state_->alive)return UIA_E_ELEMENTNOTAVAILABLE;
        if(!PostMessageW(state_->window,state_->message,0,reinterpret_cast<LPARAM>(request.get())))return HRESULT_FROM_WIN32(GetLastError());
        request.release(); return S_OK;
    }
    IRawElementProviderFragment* fragment(int id) {
        if(id==0){root()->AddRef();return static_cast<IRawElementProviderFragment*>(root());}
        return static_cast<IRawElementProviderFragment*>(new Provider(state_,id,root()));
    }
public:
    Provider(std::shared_ptr<State> state,int id=0,Provider* root=nullptr):state_(std::move(state)),id_(id),root_(root){if(root_)root_->AddRef();}
    ~Provider(){if(root_)root_->Release();}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--references_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER; *result=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IRawElementProviderSimple))*result=static_cast<IRawElementProviderSimple*>(this);
        else if(iid==__uuidof(IRawElementProviderFragment))*result=static_cast<IRawElementProviderFragment*>(this);
        else if(iid==__uuidof(IRawElementProviderFragmentRoot)&&id_==0)*result=static_cast<IRawElementProviderFragmentRoot*>(this);
        else if(iid==__uuidof(IValueProvider)){const auto n=node();if(n&&n->kind==AccessibleKind::edit)*result=static_cast<IValueProvider*>(this);}
        else if(iid==__uuidof(IInvokeProvider)){const auto n=node();if(n&&n->kind==AccessibleKind::button)*result=static_cast<IInvokeProvider*>(this);}
        if(!*result)return E_NOINTERFACE; AddRef();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override {
        if(!result)return E_POINTER;*result=ProviderOptions_ServerSideProvider;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern,IUnknown** result) override {
        if(!result)return E_POINTER;*result=nullptr;const auto n=node();if(!n)return UIA_E_ELEMENTNOTAVAILABLE;
        if(pattern==UIA_ValuePatternId&&n->kind==AccessibleKind::edit){*result=static_cast<IValueProvider*>(this);AddRef();}
        else if(pattern==UIA_InvokePatternId&&n->kind==AccessibleKind::button){*result=static_cast<IInvokeProvider*>(this);AddRef();}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,VARIANT* result) override {
        if(!result)return E_POINTER;VariantInit(result);const auto n=node();if(!n)return UIA_E_ELEMENTNOTAVAILABLE;
        switch(property){
        case UIA_NamePropertyId:result->vt=VT_BSTR;result->bstrVal=SysAllocString(n->name.c_str());break;
        case UIA_AutomationIdPropertyId:{auto id=L"gsdl-"+std::to_wstring(id_);result->vt=VT_BSTR;result->bstrVal=SysAllocString(id.c_str());break;}
        case UIA_FrameworkIdPropertyId:result->vt=VT_BSTR;result->bstrVal=SysAllocString(L"3DGSDownloader.Native");break;
        case UIA_ControlTypePropertyId:result->vt=VT_I4;result->lVal=id_==0?UIA_WindowControlTypeId:control_type(n->kind);break;
        case UIA_IsEnabledPropertyId:result->vt=VT_BOOL;result->boolVal=n->enabled?VARIANT_TRUE:VARIANT_FALSE;break;
        case UIA_HasKeyboardFocusPropertyId:result->vt=VT_BOOL;result->boolVal=n->focused?VARIANT_TRUE:VARIANT_FALSE;break;
        case UIA_BoundingRectanglePropertyId:*result=rectangle_value(n->screen_bounds);break;
        case UIA_IsKeyboardFocusablePropertyId:result->vt=VT_BOOL;result->boolVal=n->enabled&&n->kind!=AccessibleKind::text?VARIANT_TRUE:VARIANT_FALSE;break;
        case UIA_IsControlElementPropertyId:case UIA_IsContentElementPropertyId:result->vt=VT_BOOL;result->boolVal=VARIANT_TRUE;break;
        case UIA_ValueValuePropertyId:if(n->kind==AccessibleKind::edit){result->vt=VT_BSTR;result->bstrVal=SysAllocString(n->value.c_str());}break;
        case UIA_ValueIsReadOnlyPropertyId:if(n->kind==AccessibleKind::edit)*result=boolean_value(!n->enabled);break;
        default:break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if(!result)return E_POINTER;*result=nullptr;if(id_)return S_OK;
        HWND window{};
        {std::lock_guard lock(state_->mutex);if(!state_->alive)return UIA_E_ELEMENTNOTAVAILABLE;window=state_->window;}
        return UiaHostProviderFromHwnd(window,result);
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,IRawElementProviderFragment** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        int next=-1;
        {std::lock_guard lock(state_->mutex);if(!state_->alive)return UIA_E_ELEMENTNOTAVAILABLE;
            const auto& nodes=state_->nodes;
            if(id_==0){if(!nodes.empty()){if(direction==NavigateDirection_FirstChild)next=nodes.front().id;else if(direction==NavigateDirection_LastChild)next=nodes.back().id;}}
            else if(direction==NavigateDirection_Parent)next=0;
            else {const auto it=std::find_if(nodes.begin(),nodes.end(),[&](const auto& n){return n.id==id_;});
                if(it==nodes.end())return UIA_E_ELEMENTNOTAVAILABLE;
                if(direction==NavigateDirection_NextSibling&&it+1!=nodes.end())next=(it+1)->id;
                if(direction==NavigateDirection_PreviousSibling&&it!=nodes.begin())next=(it-1)->id;
            }
        }
        if(next>=0)*result=fragment(next);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
        if(!result)return E_POINTER;*result=nullptr;if(id_==0)return S_OK;
        int ids[]{UiaAppendRuntimeId,id_};*result=SafeArrayCreateVector(VT_I4,0,2);if(!*result)return E_OUTOFMEMORY;
        for(LONG i=0;i<2;++i)SafeArrayPutElement(*result,&i,&ids[i]);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override {
        if(!result)return E_POINTER;const auto n=node();if(!n)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto r=n->screen_bounds;*result={double(r.left),double(r.top),double(r.right-r.left),double(r.bottom-r.top)};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override{if(!result)return E_POINTER;*result=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetFocus() override{return post(AccessibleRequestKind::focus);}
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override{
        if(!result)return E_POINTER;*result=static_cast<IRawElementProviderFragmentRoot*>(root());root()->AddRef();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x,double y,IRawElementProviderFragment** result) override{
        if(!result)return E_POINTER;*result=nullptr;int id=0;
        {std::lock_guard lock(state_->mutex);if(!state_->alive)return UIA_E_ELEMENTNOTAVAILABLE;
            for(const auto& n:state_->nodes){auto r=n.screen_bounds;if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom){id=n.id;break;}}
        }
        *result=fragment(id);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override{
        if(!result)return E_POINTER;*result=nullptr;int id=-1;
        {std::lock_guard lock(state_->mutex);if(!state_->alive)return UIA_E_ELEMENTNOTAVAILABLE;for(const auto& n:state_->nodes)if(n.focused){id=n.id;break;}}
        if(id>=0)*result=fragment(id);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override{
        if(!value)return E_INVALIDARG;if(wcsnlen_s(value,32769)>32768)return E_INVALIDARG;return post(AccessibleRequestKind::set_value,value);
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override{
        if(!result)return E_POINTER;*result=nullptr;const auto n=node();if(!n)return UIA_E_ELEMENTNOTAVAILABLE;*result=SysAllocString(n->value.c_str());return *result?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override{if(!result)return E_POINTER;const auto n=node();if(!n)return UIA_E_ELEMENTNOTAVAILABLE;*result=!n->enabled;return S_OK;}
    HRESULT STDMETHODCALLTYPE Invoke() override{return post(AccessibleRequestKind::invoke);}
};
}
struct AccessibilityHost::Impl {
    std::shared_ptr<State> state;
    Provider* root{};
};
AccessibilityHost::AccessibilityHost(HWND window,UINT message):impl_(std::make_unique<Impl>()){
    impl_->state=std::make_shared<State>();impl_->state->window=window;impl_->state->message=message;impl_->root=new Provider(impl_->state);
}
AccessibilityHost::~AccessibilityHost(){disconnect();impl_->root->Release();}
void AccessibilityHost::update(std::vector<AccessibleNode> nodes){
    std::vector<AccessibleNode> before;
    {
        std::lock_guard lock(impl_->state->mutex);
        if(!impl_->state->alive)return;
        const auto &previous=impl_->state->nodes;
        if(previous.size()==nodes.size() && std::equal(previous.begin(),previous.end(),nodes.begin(),same_node))return;
        before=std::move(impl_->state->nodes);
        impl_->state->nodes=nodes;
    }
    // UIA events can synchronously re-enter this provider. Never raise them while
    // holding State::mutex. A render heartbeat with identical nodes emits none.
    if(!UiaClientsAreListening())return;
    const bool structure=before.size()!=nodes.size() ||
        !std::equal(before.begin(),before.end(),nodes.begin(),[](const auto &a,const auto &b){return a.id==b.id && a.kind==b.kind;});
    if(structure)UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(impl_->root),StructureChangeType_ChildrenInvalidated,nullptr,0);
    int oldFocus=-1;
    for(const auto &node:before)if(node.focused){oldFocus=node.id;break;}
    for(const auto &node:nodes){
        const auto old=std::find_if(before.begin(),before.end(),[&](const auto &value){return value.id==node.id;});
        const bool retained=old!=before.end() && old->kind==node.kind;
        const bool focusChanged=node.focused && (oldFocus!=node.id || !retained);
        // Structure invalidation already describes newly added controls. Emit
        // property deltas only for retained identities, focus for the new target.
        if(!retained && !focusChanged)continue;
        if(retained && same_node(*old,node) && !focusChanged)continue;
        auto *provider=new Provider(impl_->state,node.id,impl_->root);
        auto *simple=static_cast<IRawElementProviderSimple*>(provider);
        if(retained){
            if(old->name!=node.name)property_changed(simple,UIA_NamePropertyId,string_value(old->name),string_value(node.name));
            if(node.kind==AccessibleKind::edit && old->value!=node.value)property_changed(simple,UIA_ValueValuePropertyId,string_value(old->value),string_value(node.value));
            if(old->enabled!=node.enabled){
                property_changed(simple,UIA_IsEnabledPropertyId,boolean_value(old->enabled),boolean_value(node.enabled));
                if(node.kind!=AccessibleKind::text)property_changed(simple,UIA_IsKeyboardFocusablePropertyId,boolean_value(old->enabled),boolean_value(node.enabled));
                if(node.kind==AccessibleKind::edit)property_changed(simple,UIA_ValueIsReadOnlyPropertyId,boolean_value(!old->enabled),boolean_value(!node.enabled));
            }
            if(old->focused!=node.focused)property_changed(simple,UIA_HasKeyboardFocusPropertyId,boolean_value(old->focused),boolean_value(node.focused));
            if(!same_bounds(old->screen_bounds,node.screen_bounds))property_changed(simple,UIA_BoundingRectanglePropertyId,rectangle_value(old->screen_bounds),rectangle_value(node.screen_bounds));
        }
        if(focusChanged)UiaRaiseAutomationEvent(simple,UIA_AutomationFocusChangedEventId);
        // The child holds a reference to the root, and the shared State lives as
        // long as either UIA or this event notification retains the provider.
        provider->Release();
    }
}
LRESULT AccessibilityHost::get_object(WPARAM wparam,LPARAM lparam){
    if(static_cast<LONG>(lparam)!=UiaRootObjectId)return 0;
    return UiaReturnRawElementProvider(impl_->state->window,wparam,lparam,static_cast<IRawElementProviderSimple*>(impl_->root));
}
void AccessibilityHost::disconnect(){
    const auto state=impl_->state;
    {std::lock_guard lock(state->mutex);state->alive=false;state->nodes.clear();}
    MSG message{};
    while(PeekMessageW(&message,state->window,state->message,state->message,PM_REMOVE))delete reinterpret_cast<AccessibleRequest*>(message.lParam);
}
}

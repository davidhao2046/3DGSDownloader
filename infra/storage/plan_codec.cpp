#include "plan_storage.hpp"
#include <windows.h>
#include <charconv>
#include <limits>
#include <map>
#include <variant>

namespace gsdl::storage {
namespace {
[[noreturn]] void invalid(std::wstring message) {
    throw StorageError({std::move(message), "decode", 0, {}});
}
bool valid_utf8(std::string_view text) {
    if (text.empty()) return true;
    if (text.size() > static_cast<std::size_t>(INT_MAX)) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0) > 0;
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    if (!valid_utf8(text)) invalid(L"计划包含无效 UTF-8；当前计划与文件保持不变。");
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), out.data(), n);
    return out;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > 65536) throw StorageError({L"计划文本字段过长。", "encode", 0, {}});
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw StorageError({L"计划包含无效 UTF-16 文本。", "encode", 0, {}});
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    std::variant<std::nullptr_t, bool, std::uint64_t, std::string, Array, Object> value;
};
class Parser {
    std::string_view input_;
    std::size_t pos_{}, nodes_{};
    [[noreturn]] void fail() const { invalid(L"计划 JSON 格式错误、字段重复或超出读取限制；原文件保持不变。"); }
    void ws() { while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\r' || input_[pos_] == '\n' || input_[pos_] == '\t')) ++pos_; }
    bool consume(char c) { ws(); if (pos_ < input_.size() && input_[pos_] == c) { ++pos_; return true; } return false; }
    std::uint32_t hex4() {
        if (pos_ + 4 > input_.size()) fail();
        std::uint32_t code{};
        for (int i=0; i<4; ++i) {
            const char c=input_[pos_++];
            code <<= 4;
            if(c>='0' && c<='9') code += c-'0';
            else if(c>='a' && c<='f') code += c-'a'+10;
            else if(c>='A' && c<='F') code += c-'A'+10;
            else fail();
        }
        return code;
    }
    void codepoint(std::string& out, std::uint32_t c) {
        if(c<0x80) out += static_cast<char>(c);
        else if(c<0x800) {out += static_cast<char>(0xc0|(c>>6));out += static_cast<char>(0x80|(c&63));}
        else if(c<0x10000) {out += static_cast<char>(0xe0|(c>>12));out += static_cast<char>(0x80|((c>>6)&63));out += static_cast<char>(0x80|(c&63));}
        else {out += static_cast<char>(0xf0|(c>>18));out += static_cast<char>(0x80|((c>>12)&63));out += static_cast<char>(0x80|((c>>6)&63));out += static_cast<char>(0x80|(c&63));}
    }
    std::string string() {
        if (!consume('"')) fail();
        std::string out;
        bool ended{};
        while(pos_ < input_.size()) {
            const auto c=static_cast<unsigned char>(input_[pos_++]);
            if(c=='"') {ended=true;break;}
            if(c<0x20) fail();
            if(c!='\\') out += static_cast<char>(c);
            else {
                if(pos_==input_.size()) fail();
                switch(input_[pos_++]) {
                case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
                case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;
                case 'r':out+='\r';break;case 't':out+='\t';break;
                case 'u': {
                    auto cp=hex4();
                    if(cp>=0xd800 && cp<=0xdbff) {
                        if(pos_+2>input_.size() || input_[pos_]!='\\' || input_[pos_+1]!='u') fail();
                        pos_+=2; const auto lo=hex4();
                        if(lo<0xdc00 || lo>0xdfff) fail();
                        cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);
                    } else if(cp>=0xdc00 && cp<=0xdfff) fail();
                    if(cp==0) fail();
                    codepoint(out,cp);break;
                }
                default:fail();
                }
            }
            if(out.size()>65536) fail();
        }
        if(!ended) fail();
        return out;
    }
    Json value(unsigned depth) {
        ws(); if(++nodes_>120000 || depth>16 || pos_==input_.size()) fail();
        if(input_[pos_]=='"') return Json{string()};
        if(consume('{')) {
            Json::Object out;
            if(consume('}')) return Json{std::move(out)};
            do {
                auto key=string(); if(key.size()>128 || !consume(':')) fail();
                auto item=value(depth+1);
                if(!out.emplace(std::move(key),std::move(item)).second || out.size()>32) fail();
                if(consume('}')) return Json{std::move(out)};
            } while(consume(','));
            fail();
        }
        if(consume('[')) {
            Json::Array out;
            if(consume(']')) return Json{std::move(out)};
            do {
                if(out.size()>=max_resources) fail();
                out.push_back(value(depth+1));
                if(consume(']')) return Json{std::move(out)};
            } while(consume(','));
            fail();
        }
        for(auto [literal, boolean] : {std::pair{"true",true},std::pair{"false",false}}) {
            const std::string_view text(literal);
            if(input_.substr(pos_,text.size())==text) {pos_+=text.size();return Json{boolean};}
        }
        if(input_.substr(pos_,4)=="null") {pos_+=4;return Json{nullptr};}
        const auto start=pos_;
        while(pos_<input_.size() && input_[pos_]>='0' && input_[pos_]<='9') ++pos_;
        if(pos_==start || (pos_-start>1 && input_[start]=='0')) fail();
        std::uint64_t number{};
        const auto parsed=std::from_chars(input_.data()+start,input_.data()+pos_,number);
        if(parsed.ec!=std::errc{}) fail();
        return Json{number};
    }
public:
    explicit Parser(std::string_view input):input_(input){}
    Json parse() {auto out=value(0);ws();if(pos_!=input_.size())fail();return out;}
};
const Json::Object& object(const Json& value, std::initializer_list<std::string_view> fields) {
    const auto* out=std::get_if<Json::Object>(&value.value);
    if(!out || out->size()!=fields.size()) invalid(L"计划对象字段缺失、未知或结构错误；不作丢字段转换。");
    for(auto field:fields) if(!out->contains(field)) invalid(L"计划对象含未知字段或缺少必需字段；原文件保持不变。");
    return *out;
}
const Json& field(const Json::Object& o, std::string_view name) {return o.find(name)->second;}
template<class T> const T& typed(const Json& value) {
    const auto* out=std::get_if<T>(&value.value);
    if(!out) invalid(L"计划字段类型错误；不作隐式类型转换。");
    return *out;
}
const std::string& str(const Json::Object& o,std::string_view name){return typed<std::string>(field(o,name));}
std::wstring wstr(const Json::Object& o,std::string_view name){return wide(str(o,name));}
void quote(std::string& out,std::string_view value) {
    if(value.size()>65536 || !valid_utf8(value)) throw StorageError({L"计划包含无效或过长的 UTF-8 文本。","encode",0,{}});
    constexpr char hex[]="0123456789abcdef";
    out+='"';
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') {out+='\\';out+=static_cast<char>(c);}
        else if(c<0x20) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=static_cast<char>(c);
    }
    out+='"';
}
void property(std::string& out,std::string_view key,std::string_view value) {quote(out,key);out+=':';quote(out,value);}
void property(std::string& out,std::string_view key,std::wstring_view value) {property(out,key,utf8(value));}
}
StorageError::StorageError(Error error):std::runtime_error("plan storage operation failed"),detail_(std::move(error)){}

DownloadPlan decode_plan(std::string_view bytes) {
    if(bytes.size()>max_plan_bytes) invalid(L"计划超过 8 MiB 读取上限；原文件保持不变。");
    if(!valid_utf8(bytes)) invalid(L"计划不是有效 UTF-8；原文件保持不变。");
    auto json=Parser(bytes).parse();
    // Check the version before exact fields so future documents get a useful error.
    const auto* root=std::get_if<Json::Object>(&json.value);
    if(!root || !root->contains("schemaVersion")) invalid(L"缺少计划版本，不能打开此文件。");
    const auto version=typed<std::uint64_t>(field(*root,"schemaVersion"));
    if(version!=1) invalid(L"此计划版本不受支持；请使用对应版本软件，原文件保持不变。");
    const auto& o=object(json,{"schemaVersion","planId","source","outputDirectory","resources","createdUtc","modifiedUtc","internalFixture"});
    DownloadPlan plan;
    plan.planId=str(o,"planId"); plan.outputDirectory=wstr(o,"outputDirectory");
    plan.createdUtc=str(o,"createdUtc");plan.modifiedUtc=str(o,"modifiedUtc");
    plan.internalFixture=typed<bool>(field(o,"internalFixture"));
    const auto& source=object(field(o,"source"),{"provider","captureId","shareUrl","adapterVersion"});
    plan.source={str(source,"provider"),str(source,"captureId"),wstr(source,"shareUrl"),str(source,"adapterVersion")};
    for(const auto& item:typed<Json::Array>(field(o,"resources"))) {
        const auto& r=object(item,{"id","displayName","format","outputName","included","expectedBytes"});
        Resource resource{str(r,"id"),wstr(r,"displayName"),wstr(r,"format"),wstr(r,"outputName"),typed<bool>(field(r,"included")),{}};
        if(!std::holds_alternative<std::nullptr_t>(field(r,"expectedBytes").value)) resource.expectedBytes=typed<std::uint64_t>(field(r,"expectedBytes"));
        plan.resources.push_back(std::move(resource));
    }
    const auto check=validate_plan(plan);
    if(!check) throw StorageError({check.error,"validate",0,{}});
    return plan;
}
std::string encode_plan(const DownloadPlan& plan) {
    const auto check=validate_plan(plan);
    if(!check) throw StorageError({check.error,"validate",0,{}});
    if(plan.resources.size()>max_resources) throw StorageError({L"资源数超过 10000 上限。","encode",0,{}});
    std::string out="{\n\"schemaVersion\":1,";
    property(out,"planId",plan.planId);out+=",\n\"source\":{";
    property(out,"provider",plan.source.provider);out+=',';property(out,"captureId",plan.source.captureId);out+=',';
    property(out,"shareUrl",plan.source.shareUrl);out+=',';property(out,"adapterVersion",plan.source.adapterVersion);out+="},\n";
    property(out,"outputDirectory",plan.outputDirectory);out+=",\n\"resources\":[";
    bool first=true;
    for(const auto& resource:plan.resources) {
        if(!first)out+=',';first=false;out+="\n{";
        property(out,"id",resource.id);out+=',';property(out,"displayName",resource.displayName);out+=',';
        property(out,"format",resource.format);out+=',';property(out,"outputName",resource.outputName);out+=',';
        out+="\"included\":";out+=resource.included?"true":"false";out+=",\"expectedBytes\":";
        out+=resource.expectedBytes?std::to_string(*resource.expectedBytes):"null";out+='}';
        if(out.size()>max_plan_bytes) throw StorageError({L"计划超过 8 MiB 写入上限。","encode",0,{}});
    }
    out+="\n],\n";property(out,"createdUtc",plan.createdUtc);out+=',';property(out,"modifiedUtc",plan.modifiedUtc);
    out+=",\"internalFixture\":";out+=plan.internalFixture?"true":"false";out+="\n}\n";
    if(out.size()>max_plan_bytes) throw StorageError({L"计划超过 8 MiB 写入上限。","encode",0,{}});
    return out;
}
} // namespace gsdl::storage

// js/dom_binding.cpp
// Routes DOM property access / methods from JS through the WebCall ABI.

#include "malibu/js/dom_binding.h"
#include "malibu/dom/document.h"
#include "malibu/html/html_parser.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace malibu::js {

using namespace malibu::webcall;
using runtime::Value;
using vm::HeapObject;
using vm::DomNodeRef;

namespace {
std::string narrow(const std::u16string& s) { std::string r; for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r; }

malibu::NodeHandle handle_of(Value v) {
    if (v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kDomNodeRef)
        return static_cast<DomNodeRef*>(v.as_heap_ptr())->handle;
    return malibu::NodeHandle::null_handle();
}

bool is_node_value(Value v) {
    return v.is_heap_ptr() && v.as_heap_ptr()->kind == HeapObject::kDomNodeRef;
}

// Recursively clones a node within the same tree (deep when `deep`). Element →
// create_element + copy attributes; text/comment → create_text_node of its data.
malibu::NodeHandle clone_node(malibu::dom::DOMTree& tree, malibu::NodeHandle src, bool deep) {
    const malibu::dom::NodeCore* c = tree.document().core(src);
    if (!c) return malibu::NodeHandle::null_handle();
    malibu::NodeHandle copy;
    if (c->node_type == malibu::dom::kElementNode) {
        copy = tree.create_element(c->tag_name);
        for (const auto& kv : c->attributes) tree.set_attribute(copy, kv.first, kv.second);
    } else {
        copy = tree.create_text_node(c->text_content);
    }
    if (deep) {
        std::vector<malibu::NodeHandle> kids = c->children;  // copy: recursion allocates
        for (malibu::NodeHandle ch : kids) {
            malibu::NodeHandle cc = clone_node(tree, ch, true);
            if (!cc.is_null()) tree.append_child(copy, cc);
        }
    }
    return copy;
}

// True if `anc` is `node` or an ancestor of `node` (Node.contains semantics).
bool node_contains(malibu::dom::DOMTree& tree, malibu::NodeHandle anc, malibu::NodeHandle node) {
    for (malibu::NodeHandle h = node; !h.is_null(); h = tree.parent_node(h))
        if (h == anc) return true;
    return false;
}

bool is_void_element(const std::u16string& tag) {
    static const char16_t* kVoid[] = {u"area", u"base", u"br", u"col", u"embed", u"hr",
        u"img", u"input", u"link", u"meta", u"param", u"source", u"track", u"wbr"};
    for (const char16_t* v : kVoid) if (tag == v) return true;
    return false;
}

void html_escape(const std::u16string& s, std::u16string& out, bool attr) {
    for (char16_t c : s) {
        if (c == u'&') out += u"&amp;";
        else if (c == u'<' && !attr) out += u"&lt;";
        else if (c == u'>' && !attr) out += u"&gt;";
        else if (c == u'"' && attr) out += u"&quot;";
        else out += c;
    }
}

// Serializes a node subtree to HTML (`include_self` = outerHTML vs innerHTML).
void serialize_html(malibu::dom::DOMTree& tree, malibu::NodeHandle n, std::u16string& out, bool include_self) {
    const malibu::dom::NodeCore* c = tree.document().core(n);
    if (!c) return;
    if (c->node_type == malibu::dom::kTextNode) { html_escape(c->text_content, out, false); return; }
    if (c->node_type == malibu::dom::kCommentNode) { out += u"<!--" + c->text_content + u"-->"; return; }
    if (c->node_type != malibu::dom::kElementNode) {  // document/fragment: just children
        for (malibu::NodeHandle k : c->children) serialize_html(tree, k, out, true);
        return;
    }
    if (include_self) {
        out += u"<" + c->tag_name;
        for (const auto& kv : c->attributes) { out += u" " + kv.first + u"=\""; html_escape(kv.second, out, true); out += u"\""; }
        out += u">";
        if (is_void_element(c->tag_name)) return;
    }
    for (malibu::NodeHandle k : c->children) serialize_html(tree, k, out, true);
    if (include_self) out += u"</" + c->tag_name + u">";
}

// ---- IDL attribute reflection -----------------------------------------------
// Most HTML element properties simply reflect a content attribute. A single
// table drives both reading (el.href, img.src, input.value, el.hidden, ...) and
// writing (el.href = x). Used pervasively by every site/framework, so reflecting
// them broadly (not just id/className) is a large real-world correctness win.
enum class ReflKind : uint8_t { Str, Bool, Num };
struct Reflected { const char16_t* prop; ReflKind kind; const char16_t* attr; };
const Reflected* reflected_attr(const std::u16string& prop) {
    static const Reflected kTable[] = {
        // string-valued
        {u"href", ReflKind::Str, u"href"},   {u"src", ReflKind::Str, u"src"},
        {u"value", ReflKind::Str, u"value"}, {u"title", ReflKind::Str, u"title"},
        {u"alt", ReflKind::Str, u"alt"},     {u"type", ReflKind::Str, u"type"},
        {u"name", ReflKind::Str, u"name"},   {u"placeholder", ReflKind::Str, u"placeholder"},
        {u"lang", ReflKind::Str, u"lang"},   {u"dir", ReflKind::Str, u"dir"},
        {u"rel", ReflKind::Str, u"rel"},     {u"target", ReflKind::Str, u"target"},
        {u"htmlFor", ReflKind::Str, u"for"}, {u"action", ReflKind::Str, u"action"},
        {u"method", ReflKind::Str, u"method"}, {u"content", ReflKind::Str, u"content"},
        {u"cite", ReflKind::Str, u"cite"},   {u"label", ReflKind::Str, u"label"},
        {u"media", ReflKind::Str, u"media"}, {u"sizes", ReflKind::Str, u"sizes"},
        {u"srcset", ReflKind::Str, u"srcset"}, {u"accept", ReflKind::Str, u"accept"},
        {u"autocomplete", ReflKind::Str, u"autocomplete"}, {u"enctype", ReflKind::Str, u"enctype"},
        {u"pattern", ReflKind::Str, u"pattern"}, {u"step", ReflKind::Str, u"step"},
        {u"min", ReflKind::Str, u"min"},     {u"max", ReflKind::Str, u"max"},
        {u"htmlType", ReflKind::Str, u"type"}, {u"download", ReflKind::Str, u"download"},
        {u"coords", ReflKind::Str, u"coords"}, {u"shape", ReflKind::Str, u"shape"},
        {u"role", ReflKind::Str, u"role"},
        // boolean-valued (present ⇔ true)
        {u"hidden", ReflKind::Bool, u"hidden"}, {u"disabled", ReflKind::Bool, u"disabled"},
        {u"checked", ReflKind::Bool, u"checked"}, {u"selected", ReflKind::Bool, u"selected"},
        {u"readOnly", ReflKind::Bool, u"readonly"}, {u"required", ReflKind::Bool, u"required"},
        {u"multiple", ReflKind::Bool, u"multiple"}, {u"autofocus", ReflKind::Bool, u"autofocus"},
        {u"open", ReflKind::Bool, u"open"}, {u"controls", ReflKind::Bool, u"controls"},
        {u"loop", ReflKind::Bool, u"loop"}, {u"muted", ReflKind::Bool, u"muted"},
        {u"autoplay", ReflKind::Bool, u"autoplay"}, {u"async", ReflKind::Bool, u"async"},
        {u"defer", ReflKind::Bool, u"defer"}, {u"noValidate", ReflKind::Bool, u"novalidate"},
        {u"reversed", ReflKind::Bool, u"reversed"}, {u"default", ReflKind::Bool, u"default"},
        // numeric (long)
        {u"tabIndex", ReflKind::Num, u"tabindex"}, {u"maxLength", ReflKind::Num, u"maxlength"},
        {u"cols", ReflKind::Num, u"cols"}, {u"rows", ReflKind::Num, u"rows"},
        {u"size", ReflKind::Num, u"size"}, {u"span", ReflKind::Num, u"span"},
        {u"colSpan", ReflKind::Num, u"colspan"}, {u"rowSpan", ReflKind::Num, u"rowspan"},
    };
    for (const Reflected& r : kTable) if (prop == r.prop) return &r;
    return nullptr;
}

bool truthy(Value v) {
    if (v.is_bool()) return v.as_bool();
    if (v.is_int32()) return v.as_int32() != 0;
    if (v.is_double()) return v.as_double() != 0.0;
    return v.is_heap_ptr();  // objects/strings truthy; null/undefined falsy
}

bool same_ref(Value a, Value b) {
    return a.is_heap_ptr() && b.is_heap_ptr() && a.as_heap_ptr() == b.as_heap_ptr();
}

std::u16string trim16(const std::u16string& x) {
    size_t b = x.find_first_not_of(u" \t\n\r\f");
    size_t e = x.find_last_not_of(u" \t\n\r\f");
    return b == std::u16string::npos ? std::u16string() : x.substr(b, e - b + 1);
}

std::vector<std::u16string> split_ws(const std::u16string& s) {
    std::vector<std::u16string> out;
    std::u16string cur;
    for (char16_t c : s) {
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::u16string join_ws(const std::vector<std::u16string>& t) {
    std::u16string s;
    for (size_t i = 0; i < t.size(); ++i) { if (i) s += u' '; s += t[i]; }
    return s;
}

std::vector<std::pair<std::u16string, std::u16string>> parse_inline_style(const std::u16string& s) {
    std::vector<std::pair<std::u16string, std::u16string>> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t semi = s.find(u';', i);
        std::u16string decl = s.substr(i, semi == std::u16string::npos ? std::u16string::npos : semi - i);
        size_t colon = decl.find(u':');
        if (colon != std::u16string::npos) {
            std::u16string k = trim16(decl.substr(0, colon));
            std::u16string v = trim16(decl.substr(colon + 1));
            if (!k.empty()) out.push_back({k, v});
        }
        if (semi == std::u16string::npos) break;
        i = semi + 1;
    }
    return out;
}

std::u16string serialize_inline_style(const std::vector<std::pair<std::u16string, std::u16string>>& d) {
    std::u16string s;
    for (size_t i = 0; i < d.size(); ++i) { if (i) s += u' '; s += d[i].first; s += u": "; s += d[i].second; s += u';'; }
    return s;
}
}  // namespace

DomBinding::DomBinding(runtime::Interpreter& interp, malibu::dom::DOMTree& tree,
                       malibu::NodeHandle document_root)
    : interp_(interp), tree_(tree), document_root_(document_root) {
    ctx_.dom = &tree_;
    ctx_.realm = &realm_;
    // One call site per WebCall id (site id == webcall id) so every DOM access
    // from JS is dispatched through guarded fast/slow paths.
    using malibu::js::bytecode::CallSiteEntry;
    std::vector<CallSiteEntry> sites;
    for (uint32_t id : {WEBCALL_DOM_QUERY_SELECTOR, WEBCALL_DOM_CREATE_ELEMENT,
                        WEBCALL_DOM_APPEND_CHILD, WEBCALL_DOM_REMOVE_CHILD,
                        WEBCALL_DOM_REMOVE, WEBCALL_DOM_SET_TEXT_CONTENT,
                        WEBCALL_DOM_GET_TEXT_CONTENT, WEBCALL_DOM_SET_ATTRIBUTE,
                        WEBCALL_DOM_GET_ATTRIBUTE}) {
        sites.push_back(CallSiteEntry{id, id, false, 0});
    }
    call_sites_.register_function(0, sites);
}

Value DomBinding::box_to_value(const ValueBox& box) {
    switch (box.kind) {
        case ValueBox::Kind::Node:
            return box.node.is_null() ? Value::make_null() : interp_.make_dom_node(box.node);
        case ValueBox::Kind::Str:  return interp_.str(narrow(box.str));
        case ValueBox::Kind::Bool: return Value::make_bool(box.b);
        case ValueBox::Kind::Int:  return Value::make_int32(box.i);
        case ValueBox::Kind::Null: return Value::make_null();
        default:                   return Value::make_undefined();
    }
}

Value DomBinding::invoke(uint32_t webcall_id, malibu::NodeHandle target, const WebCallArgs& args) {
    ValueBox out;
    dispatch_call_site(ctx_, call_sites_, webcall_id, encode_handle(target), args, &out);
    return box_to_value(out);
}

Value DomBinding::dom_method(uint32_t webcall_id, const std::u16string& name) {
    DomBinding* self = this;
    return Value::make_heap_ptr(interp_.new_native(name,
        [self, webcall_id](runtime::Interpreter& in, Value thisv, std::vector<Value>& a) -> Value {
            malibu::NodeHandle target = handle_of(thisv);
            WebCallArgs args;
            switch (webcall_id) {
                case WEBCALL_DOM_QUERY_SELECTOR:
                case WEBCALL_DOM_CREATE_ELEMENT:
                case WEBCALL_DOM_GET_ATTRIBUTE:
                    if (!a.empty()) args.str_a = in.to_string(a[0]);
                    break;
                case WEBCALL_DOM_SET_ATTRIBUTE:
                    if (a.size() > 0) args.str_a = in.to_string(a[0]);
                    if (a.size() > 1) args.str_b = in.to_string(a[1]);
                    break;
                case WEBCALL_DOM_APPEND_CHILD:
                case WEBCALL_DOM_REMOVE_CHILD:
                    if (!a.empty()) args.node_a = handle_of(a[0]);
                    break;
                default: break;
            }
            return self->invoke(webcall_id, target, args);
        }));
}

void DomBinding::install() {
    DomBinding* self = this;

    interp_.set_dom_hooks(
        // get hook
        [self](runtime::Interpreter& in, Value node, const std::u16string& name, Value& out) -> bool {
            malibu::NodeHandle h = handle_of(node);
            // ---- CharacterData (Text / Comment / ProcessingInstruction) ----
            {
                const malibu::dom::NodeCore* nc = self->tree_.document().core(h);
                bool is_cd = nc && (nc->node_type == malibu::dom::kTextNode ||
                                    nc->node_type == malibu::dom::kCommentNode);
                if (is_cd && (name == u"data" || name == u"nodeValue")) {
                    out = in.str(nc->text_content); return true;
                }
                if (is_cd && name == u"length") { out = Value::make_int32(static_cast<int32_t>(nc->text_content.size())); return true; }
                if (is_cd && (name == u"substringData" || name == u"appendData" || name == u"insertData" ||
                              name == u"deleteData" || name == u"replaceData")) {
                    std::u16string mname = name;
                    out = Value::make_heap_ptr(in.new_native(name,
                        [self, h, mname](runtime::Interpreter& i2, Value, std::vector<Value>& a) -> Value {
                            const malibu::dom::NodeCore* c = self->tree_.document().core(h);
                            std::u16string d = c ? c->text_content : std::u16string();
                            size_t u32len = d.size();
                            // WebIDL unsigned long: NaN/Infinity → 0, then modulo 2^32.
                            auto u32 = [](double x) -> double { if (!(x == x) || x == std::numeric_limits<double>::infinity() || x == -std::numeric_limits<double>::infinity()) return 0; double m = std::fmod(std::trunc(x), 4294967296.0); if (m < 0) m += 4294967296.0; return m; };
                            if (mname == u"appendData") { d += a.empty() ? std::u16string() : i2.to_string(a[0]); self->tree_.set_text_content(h, d); return Value::make_undefined(); }
                            double offd = u32(a.empty() ? 0 : i2.to_number(a[0]));
                            if (offd > static_cast<double>(u32len)) i2.throw_error(u"IndexSizeError", u"offset out of bounds");
                            size_t off = static_cast<size_t>(offd);
                            double cnt = (a.size() > 1) ? u32(i2.to_number(a[1])) : 0;
                            size_t count = std::min<size_t>(static_cast<size_t>(cnt), u32len - off);
                            if (mname == u"substringData") return i2.str(d.substr(off, count));
                            if (mname == u"insertData") { d.insert(off, a.size() > 1 ? i2.to_string(a[1]) : std::u16string()); self->tree_.set_text_content(h, d); return Value::make_undefined(); }
                            // deleteData / replaceData
                            d.erase(off, count);
                            if (mname == u"replaceData") d.insert(off, a.size() > 2 ? i2.to_string(a[2]) : std::u16string());
                            self->tree_.set_text_content(h, d);
                            return Value::make_undefined();
                        }));
                    return true;
                }
            }
            if (name == u"textContent") {
                out = self->invoke(WEBCALL_DOM_GET_TEXT_CONTENT, h, {});
                return true;
            }
            if (name == u"innerHTML" || name == u"outerHTML") {
                std::u16string s; serialize_html(self->tree_, h, s, name == u"outerHTML");
                out = in.str(s);
                return true;
            }
            if (name == u"insertAdjacentHTML") {
                out = Value::make_heap_ptr(in.new_native(u"insertAdjacentHTML",
                    [self](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        if (a.size() < 2) return Value::make_undefined();
                        std::u16string pos = i2.to_string(a[0]); for (auto& c : pos) if (c>=u'A'&&c<=u'Z') c+=32;
                        malibu::NodeHandle me = handle_of(thisv);
                        malibu::NodeHandle frag = self->tree_.create_document_fragment();
                        malibu::html::HTMLParser().parse_fragment(i2.to_string(a[1]), self->tree_, frag);
                        auto* fc = self->tree_.document().core(frag);
                        std::vector<malibu::NodeHandle> kids = fc ? fc->children : std::vector<malibu::NodeHandle>{};
                        malibu::NodeHandle p = self->tree_.parent_node(me);
                        if (pos == u"beforebegin" && !p.is_null())      for (auto k : kids) self->tree_.insert_before(p, k, me);
                        else if (pos == u"afterend" && !p.is_null())    { malibu::NodeHandle ref = self->tree_.next_sibling(me);
                            for (auto k : kids) self->tree_.insert_before(p, k, ref); }
                        else if (pos == u"afterbegin")                  { malibu::NodeHandle first = self->tree_.first_child(me);
                            for (auto k : kids) self->tree_.insert_before(me, k, first); }
                        else /* beforeend */                            for (auto k : kids) self->tree_.append_child(me, k);
                        return Value::make_undefined();
                    }));
                return true;
            }
            if (name == u"id") {
                WebCallArgs a; a.str_a = u"id";
                out = self->invoke(WEBCALL_DOM_GET_ATTRIBUTE, h, a);
                return true;
            }
            if (name == u"className") {
                WebCallArgs a; a.str_a = u"class";
                out = self->invoke(WEBCALL_DOM_GET_ATTRIBUTE, h, a);
                return true;
            }
            if (name == u"nodeType") {
                const malibu::dom::NodeCore* c = self->tree_.document().core(h);
                out = Value::make_int32(c ? static_cast<int32_t>(c->node_type) : 0);
                return true;
            }
            if (name == u"isConnected") { out = Value::make_bool(self->tree_.is_connected(h)); return true; }
            if (name == u"tagName" || name == u"nodeName") {
                const malibu::dom::NodeCore* c = self->tree_.document().core(h);
                if (c && name == u"nodeName") {   // nodeName per node type
                    if (c->node_type == malibu::dom::kTextNode) { out = in.str(u"#text"); return true; }
                    if (c->node_type == malibu::dom::kCommentNode) { out = in.str(u"#comment"); return true; }
                    if (c->node_type == malibu::dom::kDocumentNode) { out = in.str(u"#document"); return true; }
                }
                std::u16string tag = c ? c->tag_name : std::u16string();
                for (auto& ch : tag) if (ch >= u'a' && ch <= u'z') ch = ch - u'a' + u'A';
                out = in.str(tag);
                return true;
            }
            if (name == u"querySelector")   { out = self->dom_method(WEBCALL_DOM_QUERY_SELECTOR, name); return true; }
            if (name == u"createElement")   { out = self->dom_method(WEBCALL_DOM_CREATE_ELEMENT, name); return true; }
            if (name == u"createElementNS" || name == u"createDocumentFragment") {
                bool frag = (name == u"createDocumentFragment");
                out = Value::make_heap_ptr(in.new_native(name,
                    [self, frag](runtime::Interpreter& i2, Value, std::vector<Value>& a) -> Value {
                        // createElementNS(ns, qualifiedName): use the local part of the
                        // qualified name (strip any "prefix:"). Fragment ≈ a neutral container.
                        if (frag) return i2.make_dom_node(self->tree_.create_document_fragment());
                        std::u16string qn = a.size() > 1 ? i2.to_string(a[1]) : std::u16string();
                        auto colon = qn.find(u':'); if (colon != std::u16string::npos) qn = qn.substr(colon + 1);
                        for (auto& ch : qn) if (ch >= u'A' && ch <= u'Z') ch = ch - u'A' + u'a';
                        return i2.make_dom_node(self->tree_.create_element(qn));
                    }));
                return true;
            }
            if (name == u"createTextNode" || name == u"createComment") {
                bool comment = (name == u"createComment");
                out = Value::make_heap_ptr(in.new_native(name,
                    [self, comment](runtime::Interpreter& i2, Value, std::vector<Value>& a) -> Value {
                        std::u16string txt = a.empty() ? std::u16string() : i2.to_string(a[0]);
                        (void)comment;  // comment nodes approximated as text for now (data methods work)
                        return i2.make_dom_node(self->tree_.create_text_node(txt));
                    }));
                return true;
            }
            if (name == u"appendChild")     { out = self->dom_method(WEBCALL_DOM_APPEND_CHILD, name); return true; }
            if (name == u"removeChild")     { out = self->dom_method(WEBCALL_DOM_REMOVE_CHILD, name); return true; }
            if (name == u"remove")          { out = self->dom_method(WEBCALL_DOM_REMOVE, name); return true; }

            // ---- ParentNode / ChildNode mutation methods + insertBefore /
            // replaceChild / cloneNode / contains / getRootNode / hasChildNodes.
            // These are used pervasively by real-world JS (el.append(), el.remove(),
            // el.before(), el.replaceWith(), node.cloneNode(true), ...). Each arg
            // that is not a DOM node is inserted as a Text node of its string value. ----
            if (name == u"append" || name == u"prepend" || name == u"before" || name == u"after" ||
                name == u"replaceWith" || name == u"replaceChildren" || name == u"insertBefore" ||
                name == u"replaceChild" || name == u"cloneNode" || name == u"contains" ||
                name == u"getRootNode" || name == u"hasChildNodes" || name == u"normalize") {
                std::u16string mname = name;
                out = Value::make_heap_ptr(in.new_native(mname,
                    [self, mname](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        auto& tree = self->tree_;
                        malibu::NodeHandle me = handle_of(thisv);
                        auto to_node = [&](Value v) -> malibu::NodeHandle {
                            if (is_node_value(v)) return handle_of(v);
                            return tree.create_text_node(i2.to_string(v));
                        };
                        if (mname == u"append") {
                            for (Value v : a) tree.append_child(me, to_node(v));
                        } else if (mname == u"prepend") {
                            malibu::NodeHandle first = tree.first_child(me);
                            for (Value v : a) tree.insert_before(me, to_node(v), first);
                        } else if (mname == u"replaceChildren") {
                            for (malibu::NodeHandle c : tree.child_nodes(me)) tree.remove_child(me, c);
                            for (Value v : a) tree.append_child(me, to_node(v));
                        } else if (mname == u"before") {
                            malibu::NodeHandle p = tree.parent_node(me);
                            if (!p.is_null()) for (Value v : a) tree.insert_before(p, to_node(v), me);
                        } else if (mname == u"after") {
                            malibu::NodeHandle p = tree.parent_node(me);
                            if (!p.is_null()) { malibu::NodeHandle ref = tree.next_sibling(me);
                                for (Value v : a) tree.insert_before(p, to_node(v), ref); }
                        } else if (mname == u"replaceWith") {
                            malibu::NodeHandle p = tree.parent_node(me);
                            if (!p.is_null()) { for (Value v : a) tree.insert_before(p, to_node(v), me);
                                tree.remove_child(p, me); }
                        } else if (mname == u"insertBefore") {
                            // insertBefore(newNode, refNode)
                            malibu::NodeHandle nn = a.empty() ? malibu::NodeHandle::null_handle() : to_node(a[0]);
                            malibu::NodeHandle ref = (a.size() > 1 && is_node_value(a[1])) ? handle_of(a[1]) : malibu::NodeHandle::null_handle();
                            tree.insert_before(me, nn, ref);
                            return a.empty() ? Value::make_null() : a[0];
                        } else if (mname == u"replaceChild") {
                            // replaceChild(newChild, oldChild) → returns oldChild
                            if (a.size() >= 2 && is_node_value(a[1])) {
                                malibu::NodeHandle nn = to_node(a[0]), old = handle_of(a[1]);
                                tree.insert_before(me, nn, old); tree.remove_child(me, old);
                                return a[1];
                            }
                            return Value::make_null();
                        } else if (mname == u"cloneNode") {
                            bool deep = !a.empty() && truthy(a[0]);
                            return i2.make_dom_node(clone_node(tree, me, deep));
                        } else if (mname == u"contains") {
                            if (a.empty() || !is_node_value(a[0])) return Value::make_bool(false);
                            return Value::make_bool(node_contains(tree, me, handle_of(a[0])));
                        } else if (mname == u"getRootNode") {
                            malibu::NodeHandle r = me, p;
                            while (!(p = tree.parent_node(r)).is_null()) r = p;
                            return i2.make_dom_node(r);
                        } else if (mname == u"hasChildNodes") {
                            return Value::make_bool(!tree.child_nodes(me).empty());
                        } else if (mname == u"normalize") {
                            return Value::make_undefined();  // text-node merge: no-op (no split text yet)
                        }
                        return Value::make_undefined();
                    }));
                return true;
            }
            if (name == u"setAttribute")    { out = self->dom_method(WEBCALL_DOM_SET_ATTRIBUTE, name); return true; }
            if (name == u"getAttribute")    { out = self->dom_method(WEBCALL_DOM_GET_ATTRIBUTE, name); return true; }
            if (name == u"getContext") {     // <canvas>.getContext(type) -> host-provided context
                out = Value::make_heap_ptr(in.new_native(u"getContext",
                    [self](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        if (!self->context_provider_) return Value::make_null();
                        std::u16string type = a.empty() ? u"2d" : i2.to_string(a[0]);
                        return self->context_provider_(handle_of(thisv), type);
                    }));
                return true;
            }
            // Layout geometry (getBoundingClientRect / offset* / client* / scroll*).
            if (name == u"getBoundingClientRect") {
                out = Value::make_heap_ptr(in.new_native(u"getBoundingClientRect",
                    [self, h](runtime::Interpreter& i2, Value, std::vector<Value>&) -> Value {
                        float x = 0, y = 0, w = 0, hh = 0;
                        if (self->rect_provider_) self->rect_provider_(h, x, y, w, hh);
                        auto* r = i2.new_object();
                        r->set(u"x", Value::make_double(x));      r->set(u"y", Value::make_double(y));
                        r->set(u"left", Value::make_double(x));   r->set(u"top", Value::make_double(y));
                        r->set(u"width", Value::make_double(w));  r->set(u"height", Value::make_double(hh));
                        r->set(u"right", Value::make_double(x + w)); r->set(u"bottom", Value::make_double(y + hh));
                        return Value::make_heap_ptr(r);
                    }));
                return true;
            }
            if (name == u"offsetWidth" || name == u"clientWidth" || name == u"scrollWidth" ||
                name == u"offsetHeight" || name == u"clientHeight" || name == u"scrollHeight" ||
                name == u"offsetTop" || name == u"offsetLeft") {
                float x = 0, y = 0, w = 0, hh = 0;
                if (self->rect_provider_) self->rect_provider_(h, x, y, w, hh);
                double v = (name == u"offsetWidth" || name == u"clientWidth" || name == u"scrollWidth") ? w
                         : (name == u"offsetHeight" || name == u"clientHeight" || name == u"scrollHeight") ? hh
                         : (name == u"offsetTop") ? y : x;
                out = Value::make_double(v);
                return true;
            }
            if (name == u"addEventListener") {
                out = Value::make_heap_ptr(in.new_native(u"addEventListener",
                    [self](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        if (a.size() >= 2) {
                            bool cap = false;
                            if (a.size() >= 3) {
                                if (a[2].is_bool()) cap = a[2].as_bool();
                                else if (a[2].is_heap_ptr()) cap = truthy(i2.get_prop_public(a[2], u"capture"));
                            }
                            self->add_listener(handle_of(thisv), i2.to_string(a[0]), a[1], cap);
                        }
                        return Value::make_undefined();
                    }));
                return true;
            }
            if (name == u"removeEventListener") {
                out = Value::make_heap_ptr(in.new_native(u"removeEventListener",
                    [self](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        if (a.size() >= 2) {
                            bool cap = (a.size() >= 3 && a[2].is_bool()) ? a[2].as_bool() : false;
                            self->remove_listener(handle_of(thisv), i2.to_string(a[0]), a[1], cap);
                        }
                        return Value::make_undefined();
                    }));
                return true;
            }
            if (name == u"dispatchEvent") {
                out = Value::make_heap_ptr(in.new_native(u"dispatchEvent",
                    [self](runtime::Interpreter&, Value thisv, std::vector<Value>& a) -> Value {
                        if (a.empty()) return Value::make_bool(true);
                        return Value::make_bool(self->dispatch_value(handle_of(thisv), a[0]));
                    }));
                return true;
            }
            // ---- classList / style ----
            if (name == u"classList") { out = self->make_class_list(h); return true; }
            if (name == u"style")     { out = self->make_style(h); return true; }
            if (name == u"dataset")   { out = self->make_dataset(h); return true; }
            // ---- tree traversal ----
            if (name == u"parentNode" || name == u"parentElement") {
                malibu::NodeHandle p = self->tree_.parent_node(h);
                out = p.is_null() ? Value::make_null() : in.make_dom_node(p);
                return true;
            }
            if (name == u"firstChild") {
                malibu::NodeHandle c = self->tree_.first_child(h);
                out = c.is_null() ? Value::make_null() : in.make_dom_node(c); return true;
            }
            if (name == u"lastChild") {
                malibu::NodeHandle c = self->tree_.last_child(h);
                out = c.is_null() ? Value::make_null() : in.make_dom_node(c); return true;
            }
            if (name == u"nextSibling") {
                malibu::NodeHandle c = self->tree_.next_sibling(h);
                out = c.is_null() ? Value::make_null() : in.make_dom_node(c); return true;
            }
            if (name == u"previousSibling") {
                malibu::NodeHandle c = self->tree_.previous_sibling(h);
                out = c.is_null() ? Value::make_null() : in.make_dom_node(c); return true;
            }
            if (name == u"firstElementChild" || name == u"lastElementChild") {
                auto kids = self->tree_.child_nodes(h);
                malibu::NodeHandle found = malibu::NodeHandle::null_handle();
                auto is_el = [&](malibu::NodeHandle k) {
                    auto* core = self->tree_.document().core(k);
                    return core && core->node_type == malibu::dom::kElementNode;
                };
                if (name == u"firstElementChild") {
                    for (auto k : kids) if (is_el(k)) { found = k; break; }
                } else {
                    for (auto it = kids.rbegin(); it != kids.rend(); ++it) if (is_el(*it)) { found = *it; break; }
                }
                out = found.is_null() ? Value::make_null() : in.make_dom_node(found);
                return true;
            }
            if (name == u"children" || name == u"childNodes") {
                bool elements_only = (name == u"children");
                auto* arr = in.new_array();
                for (auto c : self->tree_.child_nodes(h)) {
                    if (elements_only) {
                        auto* core = self->tree_.document().core(c);
                        if (!core || core->node_type != malibu::dom::kElementNode) continue;
                    }
                    arr->elements.push_back(in.make_dom_node(c));
                }
                out = Value::make_heap_ptr(arr); return true;
            }
            if (name == u"childElementCount") {
                int n = 0;
                for (auto c : self->tree_.child_nodes(h)) {
                    auto* core = self->tree_.document().core(c);
                    if (core && core->node_type == malibu::dom::kElementNode) ++n;
                }
                out = Value::make_int32(n); return true;
            }
            // ---- collection queries ----
            if (name == u"getElementById") {
                out = Value::make_heap_ptr(in.new_native(u"getElementById",
                    [self](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        if (a.empty()) return Value::make_null();
                        std::u16string sel = u"#"; sel += i2.to_string(a[0]);
                        malibu::NodeHandle n = self->tree_.query_selector(handle_of(thisv), sel);
                        return n.is_null() ? Value::make_null() : i2.make_dom_node(n);
                    }));
                return true;
            }
            if (name == u"getElementsByClassName" || name == u"getElementsByTagName") {
                bool by_class = (name == u"getElementsByClassName");
                out = Value::make_heap_ptr(in.new_native(name,
                    [self, by_class](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        auto* arr = i2.new_array();
                        if (!a.empty()) {
                            std::u16string sel = by_class ? (u"." + i2.to_string(a[0])) : i2.to_string(a[0]);
                            std::vector<malibu::NodeHandle> ns;
                            self->tree_.query_selector_all(handle_of(thisv), sel, ns);
                            for (auto n : ns) arr->elements.push_back(i2.make_dom_node(n));
                        }
                        return Value::make_heap_ptr(arr);
                    }));
                return true;
            }
            // ---- attribute presence / removal ----
            if (name == u"hasAttribute") {
                out = Value::make_heap_ptr(in.new_native(u"hasAttribute",
                    [self](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        if (a.empty()) return Value::make_bool(false);
                        return Value::make_bool(self->tree_.get_attribute(handle_of(thisv), i2.to_string(a[0])).has_value());
                    }));
                return true;
            }
            if (name == u"removeAttribute") {
                out = Value::make_heap_ptr(in.new_native(u"removeAttribute",
                    [self](runtime::Interpreter& i2, Value thisv, std::vector<Value>& a) -> Value {
                        if (!a.empty()) {
                            auto* core = self->tree_.document().core(handle_of(thisv));
                            if (core) {
                                std::u16string n = i2.to_string(a[0]);
                                for (auto it = core->attributes.begin(); it != core->attributes.end(); ++it)
                                    if (it->first == n) { core->attributes.erase(it); break; }
                            }
                        }
                        return Value::make_undefined();
                    }));
                return true;
            }
            if (name == u"querySelectorAll") {
                out = Value::make_heap_ptr(in.new_native(u"querySelectorAll",
                    [self](runtime::Interpreter& in2, Value thisv, std::vector<Value>& a) -> Value {
                        malibu::NodeHandle scope = handle_of(thisv);
                        std::vector<malibu::NodeHandle> nodes;
                        if (!a.empty()) self->tree_.query_selector_all(scope, in2.to_string(a[0]), nodes);
                        auto* arr = in2.new_array();
                        for (auto n : nodes) arr->elements.push_back(in2.make_dom_node(n));
                        return Value::make_heap_ptr(arr);
                    }));
                return true;
            }
            // Reflected IDL attribute (href/src/value/hidden/disabled/tabIndex/...):
            // read the content attribute and coerce by kind.
            if (const Reflected* r = reflected_attr(name)) {
                auto av = self->tree_.get_attribute(h, r->attr);
                if (r->kind == ReflKind::Str)       out = in.str(av.value_or(std::u16string()));
                else if (r->kind == ReflKind::Bool) out = Value::make_bool(av.has_value());
                else { double n = av ? in.to_number(in.str(*av)) : 0.0;
                       out = Value::make_int32(std::isnan(n) ? 0 : static_cast<int32_t>(n)); }
                return true;
            }
            // Unknown property: return the stored expando if any, else undefined.
            if (node.is_heap_ptr() && node.as_heap_ptr()->kind == vm::HeapObject::kDomNodeRef) {
                auto& ex = static_cast<vm::DomNodeRef*>(node.as_heap_ptr())->expandos;
                auto it = ex.find(name);
                if (it != ex.end()) { Value vv; vv.raw = it->second; out = vv; return true; }
            }
            out = Value::make_undefined();
            return true;
        },
        // set hook
        [self](runtime::Interpreter& in, Value node, const std::u16string& name, Value v) -> bool {
            malibu::NodeHandle h = handle_of(node);
            if (name == u"textContent") {
                WebCallArgs a; a.str_a = in.to_string(v);
                self->invoke(WEBCALL_DOM_SET_TEXT_CONTENT, h, a);
                return true;
            }
            if (name == u"innerHTML") {
                // Replace all children with the parsed fragment (scripts do NOT run).
                if (auto* core = self->tree_.document().core(h)) {
                    for (malibu::NodeHandle c : std::vector<malibu::NodeHandle>(core->children))
                        self->tree_.remove_child(h, c);
                }
                malibu::html::HTMLParser().parse_fragment(in.to_string(v), self->tree_, h);
                return true;
            }
            if (name == u"outerHTML") {
                // Parse into the parent at this node's position, then remove this node.
                malibu::NodeHandle p = self->tree_.parent_node(h);
                if (!p.is_null()) {
                    auto* frag = self->tree_.document().core(
                        [&]{ malibu::NodeHandle f = self->tree_.create_document_fragment();
                             malibu::html::HTMLParser().parse_fragment(in.to_string(v), self->tree_, f);
                             return f; }());
                    if (frag) for (malibu::NodeHandle c : std::vector<malibu::NodeHandle>(frag->children))
                        self->tree_.insert_before(p, c, h);
                    self->tree_.remove_child(p, h);
                }
                return true;
            }
            if (name == u"id" || name == u"className") {
                WebCallArgs a; a.str_a = (name == u"id") ? u"id" : u"class"; a.str_b = in.to_string(v);
                self->invoke(WEBCALL_DOM_SET_ATTRIBUTE, h, a);
                return true;
            }
            // Reflected IDL attribute assignment (el.href = x, el.hidden = true,
            // input.value = "...", el.tabIndex = 3): write/remove the content
            // attribute per the reflection kind.
            if (const Reflected* r = reflected_attr(name)) {
                if (r->kind == ReflKind::Bool) {
                    if (truthy(v)) { WebCallArgs a; a.str_a = r->attr; a.str_b = u""; self->invoke(WEBCALL_DOM_SET_ATTRIBUTE, h, a); }
                    else if (auto* core = self->tree_.document().core(h)) {
                        std::u16string an = r->attr;
                        for (auto it = core->attributes.begin(); it != core->attributes.end(); ++it)
                            if (it->first == an) { core->attributes.erase(it); break; }
                    }
                } else {
                    WebCallArgs a; a.str_a = r->attr; a.str_b = in.to_string(v);
                    self->invoke(WEBCALL_DOM_SET_ATTRIBUTE, h, a);
                }
                return true;
            }
            // Unknown property → store as an expando (DOM nodes are objects and
            // hold arbitrary JS-assigned properties).
            if (node.is_heap_ptr() && node.as_heap_ptr()->kind == vm::HeapObject::kDomNodeRef)
                static_cast<vm::DomNodeRef*>(node.as_heap_ptr())->expandos[name] = v.raw;
            return true;
        });

    // Event constructor: new Event(type, { bubbles, cancelable })
    auto* event_ctor = interp_.new_native(u"Event",
        [self](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::u16string type = a.empty() ? std::u16string() : in.to_string(a[0]);
            bool bubbles = false, cancelable = false;
            if (a.size() > 1 && a[1].is_heap_ptr()) {
                bubbles = truthy(in.get_prop_public(a[1], u"bubbles"));
                cancelable = truthy(in.get_prop_public(a[1], u"cancelable"));
            }
            return self->make_event(type, bubbles, cancelable);
        });
    interp_.global()->define(u"Event", Value::make_heap_ptr(event_ctor));

    interp_.global()->define(u"document", interp_.make_dom_node(document_root_));
}

// ---------------------------------------------------------------------------
// EventTarget + event dispatch (WHATWG DOM)
// ---------------------------------------------------------------------------
DomBinding::~DomBinding() {
    for (auto& [key, types] : listeners_)
        for (auto& [type, vec] : types)
            for (auto& L : vec) interp_.remove_host_root(L.callback);
}

void DomBinding::add_listener(malibu::NodeHandle node, const std::u16string& type,
                              Value cb, bool capture) {
    if (node.is_null() || !interp_.is_callable(cb)) return;
    auto& vec = listeners_[encode_handle(node)][type];
    for (auto& L : vec)
        if (L.capture == capture && same_ref(L.callback, cb)) return;  // dedupe (DOM spec)
    vec.push_back(Listener{cb, capture});
    interp_.add_host_root(cb);
}

void DomBinding::remove_listener(malibu::NodeHandle node, const std::u16string& type,
                                 Value cb, bool capture) {
    auto it = listeners_.find(encode_handle(node));
    if (it == listeners_.end()) return;
    auto t = it->second.find(type);
    if (t == it->second.end()) return;
    for (auto i = t->second.begin(); i != t->second.end(); ++i) {
        if (i->capture == capture && same_ref(i->callback, cb)) {
            interp_.remove_host_root(i->callback);
            t->second.erase(i);
            return;
        }
    }
}

Value DomBinding::make_event(const std::u16string& type, bool bubbles, bool cancelable) {
    auto* obj = interp_.new_object();
    Value ev = Value::make_heap_ptr(obj);
    interp_.set_prop_public(ev, u"type", interp_.str(narrow(type)));
    interp_.set_prop_public(ev, u"bubbles", Value::make_bool(bubbles));
    interp_.set_prop_public(ev, u"cancelable", Value::make_bool(cancelable));
    interp_.set_prop_public(ev, u"defaultPrevented", Value::make_bool(false));
    interp_.set_prop_public(ev, u"eventPhase", Value::make_int32(0));
    interp_.set_prop_public(ev, u"target", Value::make_null());
    interp_.set_prop_public(ev, u"currentTarget", Value::make_null());
    interp_.set_prop_public(ev, u"__stop", Value::make_bool(false));
    interp_.set_prop_public(ev, u"__stopNow", Value::make_bool(false));
    interp_.set_prop_public(ev, u"preventDefault", Value::make_heap_ptr(interp_.new_native(
        u"preventDefault", [](runtime::Interpreter& in, Value t, std::vector<Value>&) -> Value {
            if (truthy(in.get_prop_public(t, u"cancelable")))
                in.set_prop_public(t, u"defaultPrevented", Value::make_bool(true));
            return Value::make_undefined();
        })));
    interp_.set_prop_public(ev, u"stopPropagation", Value::make_heap_ptr(interp_.new_native(
        u"stopPropagation", [](runtime::Interpreter& in, Value t, std::vector<Value>&) -> Value {
            in.set_prop_public(t, u"__stop", Value::make_bool(true));
            return Value::make_undefined();
        })));
    interp_.set_prop_public(ev, u"stopImmediatePropagation", Value::make_heap_ptr(interp_.new_native(
        u"stopImmediatePropagation", [](runtime::Interpreter& in, Value t, std::vector<Value>&) -> Value {
            in.set_prop_public(t, u"__stop", Value::make_bool(true));
            in.set_prop_public(t, u"__stopNow", Value::make_bool(true));
            return Value::make_undefined();
        })));
    return ev;
}

Value DomBinding::make_class_list(malibu::NodeHandle h) {
    auto* obj = interp_.new_object();
    Value clv = Value::make_heap_ptr(obj);
    DomBinding* self = this;
    malibu::NodeHandle hh = h;
    auto read = [self, hh]() {
        auto a = self->tree_.get_attribute(hh, u"class");
        return a ? split_ws(*a) : std::vector<std::u16string>{};
    };

    interp_.set_prop_public(clv, u"contains", Value::make_heap_ptr(interp_.new_native(u"contains",
        [self, hh, read](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            if (a.empty()) return Value::make_bool(false);
            std::u16string c = in.to_string(a[0]);
            for (auto& t : read()) if (t == c) return Value::make_bool(true);
            return Value::make_bool(false);
        })));
    interp_.set_prop_public(clv, u"add", Value::make_heap_ptr(interp_.new_native(u"add",
        [self, hh, read](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            auto toks = read();
            for (auto& arg : a) {
                std::u16string c = in.to_string(arg);
                if (std::find(toks.begin(), toks.end(), c) == toks.end()) toks.push_back(c);
            }
            self->tree_.set_attribute(hh, u"class", join_ws(toks));
            return Value::make_undefined();
        })));
    interp_.set_prop_public(clv, u"remove", Value::make_heap_ptr(interp_.new_native(u"remove",
        [self, hh, read](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            auto toks = read();
            for (auto& arg : a) {
                std::u16string c = in.to_string(arg);
                toks.erase(std::remove(toks.begin(), toks.end(), c), toks.end());
            }
            self->tree_.set_attribute(hh, u"class", join_ws(toks));
            return Value::make_undefined();
        })));
    interp_.set_prop_public(clv, u"toggle", Value::make_heap_ptr(interp_.new_native(u"toggle",
        [self, hh, read](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            if (a.empty()) return Value::make_bool(false);
            std::u16string c = in.to_string(a[0]);
            bool has_force = a.size() > 1;
            bool force = has_force && a[1].is_bool() && a[1].as_bool();
            auto toks = read();
            bool present = std::find(toks.begin(), toks.end(), c) != toks.end();
            bool add = has_force ? force : !present;
            if (add && !present) toks.push_back(c);
            else if (!add && present) toks.erase(std::remove(toks.begin(), toks.end(), c), toks.end());
            self->tree_.set_attribute(hh, u"class", join_ws(toks));
            return Value::make_bool(add);
        })));
    return clv;
}

Value DomBinding::make_style(malibu::NodeHandle h) {
    auto* obj = interp_.new_object();
    Value sv = Value::make_heap_ptr(obj);
    DomBinding* self = this;
    malibu::NodeHandle hh = h;
    auto decls = [self, hh]() {
        auto a = self->tree_.get_attribute(hh, u"style");
        return a ? parse_inline_style(*a) : std::vector<std::pair<std::u16string, std::u16string>>{};
    };

    interp_.set_prop_public(sv, u"getPropertyValue", Value::make_heap_ptr(interp_.new_native(u"getPropertyValue",
        [self, hh, decls](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            if (a.empty()) return in.str("");
            std::u16string p = in.to_string(a[0]);
            for (auto& d : decls()) if (d.first == p) return in.str(narrow(d.second));
            return in.str("");
        })));
    interp_.set_prop_public(sv, u"setProperty", Value::make_heap_ptr(interp_.new_native(u"setProperty",
        [self, hh, decls](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            if (a.size() < 2) return Value::make_undefined();
            std::u16string p = in.to_string(a[0]), v = in.to_string(a[1]);
            auto d = decls();
            bool found = false;
            for (auto& kv : d) if (kv.first == p) { kv.second = v; found = true; break; }
            if (!found) d.push_back({p, v});
            self->tree_.set_attribute(hh, u"style", serialize_inline_style(d));
            return Value::make_undefined();
        })));
    interp_.set_prop_public(sv, u"removeProperty", Value::make_heap_ptr(interp_.new_native(u"removeProperty",
        [self, hh, decls](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            if (a.empty()) return Value::make_undefined();
            std::u16string p = in.to_string(a[0]);
            auto d = decls();
            d.erase(std::remove_if(d.begin(), d.end(), [&](auto& kv){ return kv.first == p; }), d.end());
            self->tree_.set_attribute(hh, u"style", serialize_inline_style(d));
            return Value::make_undefined();
        })));

    // Live camelCase property access (`el.style.backgroundColor = "red"`) is the
    // dominant CSSOM write pattern. A Proxy routes any non-method property
    // through getPropertyValue/setProperty (which write the inline `style`
    // attribute via the WebCall ABI) — the generalized host-object mechanism.
    auto camel_to_kebab = [](const std::u16string& s) {
        std::u16string out;
        for (char16_t c : s) {
            if (c >= u'A' && c <= u'Z') { out += u'-'; out += static_cast<char16_t>(c - u'A' + u'a'); }
            else out += c;
        }
        return out;
    };
    auto* handler = interp_.new_object();
    interp_.set_prop_public(Value::make_heap_ptr(handler), u"get", Value::make_heap_ptr(interp_.new_native(u"get",
        [camel_to_kebab, decls](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            Value target = a.size() > 0 ? a[0] : Value::make_undefined();
            std::u16string key = a.size() > 1 ? in.to_string(a[1]) : u"";
            if (target.is_heap_ptr() && static_cast<runtime::JSObject*>(target.as_heap_ptr())->has_own(key))
                return in.get_prop_public(target, key);   // setProperty / getPropertyValue / removeProperty
            if (key == u"cssText") return in.str(narrow(serialize_inline_style(decls())));
            std::u16string css = camel_to_kebab(key);
            for (auto& d : decls()) if (d.first == css) return in.str(narrow(d.second));
            return in.str("");
        })));
    interp_.set_prop_public(Value::make_heap_ptr(handler), u"set", Value::make_heap_ptr(interp_.new_native(u"set",
        [self, hh, camel_to_kebab, decls](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::u16string key = a.size() > 1 ? in.to_string(a[1]) : u"";
            std::u16string val = a.size() > 2 ? in.to_string(a[2]) : u"";
            if (key == u"cssText") { self->tree_.set_attribute(hh, u"style", val); return Value::make_bool(true); }
            std::u16string css = camel_to_kebab(key);
            auto d = decls();
            bool found = false;
            for (auto& kv : d) if (kv.first == css) { kv.second = val; found = true; break; }
            if (!found) d.push_back({css, val});
            self->tree_.set_attribute(hh, u"style", serialize_inline_style(d));
            return Value::make_bool(true);
        })));
    auto* px = interp_.heap().alloc<runtime::JSProxy>();
    px->target = sv;
    px->handler = Value::make_heap_ptr(handler);
    return Value::make_heap_ptr(px);
}

// el.dataset: a live map of data-* attributes. `dataset.fooBar` ⇄ attribute
// `data-foo-bar` (camelCase ⇄ hyphenated). Used pervasively by frameworks.
Value DomBinding::make_dataset(malibu::NodeHandle h) {
    DomBinding* self = this;
    malibu::NodeHandle hh = h;
    // dataset key (camelCase) → content attribute name (data-foo-bar).
    auto key_to_attr = [](const std::u16string& s) {
        std::u16string out = u"data-";
        for (char16_t c : s) {
            if (c >= u'A' && c <= u'Z') { out += u'-'; out += static_cast<char16_t>(c - u'A' + u'a'); }
            else out += c;
        }
        return out;
    };
    auto* target = interp_.new_object();
    auto* handler = interp_.new_object();
    interp_.set_prop_public(Value::make_heap_ptr(handler), u"get", Value::make_heap_ptr(interp_.new_native(u"get",
        [self, hh, key_to_attr](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::u16string key = a.size() > 1 ? in.to_string(a[1]) : u"";
            auto v = self->tree_.get_attribute(hh, key_to_attr(key));
            return v ? in.str(narrow(*v)) : Value::make_undefined();
        })));
    interp_.set_prop_public(Value::make_heap_ptr(handler), u"set", Value::make_heap_ptr(interp_.new_native(u"set",
        [self, hh, key_to_attr](runtime::Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::u16string key = a.size() > 1 ? in.to_string(a[1]) : u"";
            std::u16string val = a.size() > 2 ? in.to_string(a[2]) : u"";
            self->tree_.set_attribute(hh, key_to_attr(key), val);
            return Value::make_bool(true);
        })));
    auto* px = interp_.heap().alloc<runtime::JSProxy>();
    px->target = Value::make_heap_ptr(target);
    px->handler = Value::make_heap_ptr(handler);
    return Value::make_heap_ptr(px);
}

void DomBinding::fire_at(malibu::NodeHandle node, const std::u16string& type, Value event,
                         bool fire_capture, bool fire_bubble, bool& stop, bool& stop_now) {
    auto it = listeners_.find(encode_handle(node));
    if (it == listeners_.end()) return;
    auto t = it->second.find(type);
    if (t == it->second.end()) return;
    std::vector<Listener> list = t->second;  // copy: a handler may mutate listeners_
    Value node_val = interp_.make_dom_node(node);
    interp_.set_prop_public(event, u"currentTarget", node_val);
    for (auto& L : list) {
        bool match = (L.capture && fire_capture) || (!L.capture && fire_bubble);
        if (!match || !interp_.is_callable(L.callback)) continue;
        std::vector<Value> args{event};
        try { interp_.call(L.callback, node_val, args); }
        catch (runtime::ThrowSignal&) {}  // a listener throwing must not abort dispatch
        if (truthy(interp_.get_prop_public(event, u"__stopNow"))) { stop = true; stop_now = true; return; }
        if (truthy(interp_.get_prop_public(event, u"__stop"))) stop = true;
    }
}

bool DomBinding::dispatch_value(malibu::NodeHandle target, Value event) {
    if (target.is_null()) return true;
    std::u16string type = interp_.to_string(interp_.get_prop_public(event, u"type"));
    bool bubbles = truthy(interp_.get_prop_public(event, u"bubbles"));
    interp_.set_prop_public(event, u"target", interp_.make_dom_node(target));

    std::vector<malibu::NodeHandle> ancestors;  // target's parent → root
    for (malibu::NodeHandle p = tree_.parent_node(target); !p.is_null(); p = tree_.parent_node(p))
        ancestors.push_back(p);

    interp_.push_root(event);
    bool stop = false, stop_now = false;

    interp_.set_prop_public(event, u"eventPhase", Value::make_int32(1));  // capturing
    for (auto i = ancestors.rbegin(); i != ancestors.rend() && !stop; ++i)
        fire_at(*i, type, event, true, false, stop, stop_now);

    if (!stop) {  // at target
        interp_.set_prop_public(event, u"eventPhase", Value::make_int32(2));
        fire_at(target, type, event, true, true, stop, stop_now);
    }

    if (bubbles && !stop) {  // bubbling
        interp_.set_prop_public(event, u"eventPhase", Value::make_int32(3));
        for (auto i = ancestors.begin(); i != ancestors.end() && !stop; ++i)
            fire_at(*i, type, event, false, true, stop, stop_now);
    }

    interp_.set_prop_public(event, u"currentTarget", Value::make_null());
    interp_.set_prop_public(event, u"eventPhase", Value::make_int32(0));
    bool canceled = truthy(interp_.get_prop_public(event, u"defaultPrevented"));
    interp_.pop_root();
    interp_.run_microtasks();  // settle promises a listener may have scheduled
    return !canceled;
}

bool DomBinding::dispatch_event(malibu::NodeHandle target, const std::u16string& type,
                                bool bubbles, bool cancelable) {
    return dispatch_value(target, make_event(type, bubbles, cancelable));
}

malibu::NodeHandle DomBinding::node_of(runtime::Value v) const {
    return handle_of(v);
}

} // namespace malibu::js
